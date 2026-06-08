#include "daemon_pool.hpp"

#include "../daemon/runtime_files.hpp"
#include "../utils/constants.hpp"
#include "../utils/logger.hpp"

namespace acecode::desktop {

namespace {

constexpr std::chrono::milliseconds kSlowStartupTotalWait(60000);

std::chrono::milliseconds slow_startup_extra_wait(std::chrono::milliseconds initial_wait) {
    if (initial_wait >= kSlowStartupTotalWait) return std::chrono::milliseconds(0);
    return kSlowStartupTotalWait - initial_wait;
}

std::string ms_count(std::chrono::milliseconds value) {
    return std::to_string(value.count());
}

} // namespace

DaemonPool::~DaemonPool() {
    // 不主动 stop_all — 让调用方控制(quit 时显式调,析构时机可能在异常路径上)。
}

std::string DaemonPool::slot_key(const std::string& hash, const std::string& context_id) {
    return hash + "\n" + (context_id.empty() ? "default" : context_id);
}

DaemonPool::Slot* DaemonPool::get_or_create_slot(const std::string& hash,
                                                 const std::string& context_id) {
    std::lock_guard<std::mutex> lk(main_mu_);
    const std::string key = slot_key(hash, context_id);
    auto it = slots_.find(key);
    if (it != slots_.end()) return it->second.get();
    auto slot = std::make_unique<Slot>();
    if (factory_) {
        slot->sup = factory_();
    } else {
        slot->sup = std::make_unique<DaemonSupervisor>();
    }
    Slot* raw = slot.get();
    slots_.emplace(key, std::move(slot));
    return raw;
}

ActivateResult DaemonPool::activate(const ActivateRequest& req,
                                    std::chrono::milliseconds wait_timeout) {
    ActivateResult r;
    if (req.hash.empty() || req.cwd.empty() || req.daemon_exe_path.empty()) {
        r.error = "activate: missing hash/cwd/daemon_exe_path";
        return r;
    }

    const std::string context_id = req.context_id.empty() ? "default" : req.context_id;
    Slot* slot = get_or_create_slot(req.hash, context_id);

    // per-Slot 锁: 把"读 state / 等 cv / 触发 spawn"串成线性。
    std::unique_lock<std::mutex> sl(slot->mu);

    // Already running → 直接返回缓存
    if (slot->state == DaemonState::Running) {
        r.ok = true;
        r.port = slot->port;
        r.token = slot->token;
        return r;
    }
    // Failed → 把上次错误返回,不重试(MVP);如需重试由调用方先 stop 再 activate。
    if (slot->state == DaemonState::Failed) {
        r.error = slot->error.empty() ? "activate: previous attempt failed" : slot->error;
        return r;
    }
    // Starting → 等当前 spawn 完成
    if (slot->state == DaemonState::Starting) {
        slot->cv.wait(sl, [&] {
            return slot->state == DaemonState::Running ||
                   slot->state == DaemonState::Failed ||
                   slot->state == DaemonState::Stopped;
        });
        if (slot->state == DaemonState::Running) {
            r.ok = true;
            r.port = slot->port;
            r.token = slot->token;
        } else {
            r.error = slot->error.empty() ? "activate: peer waiter saw non-running state" : slot->error;
        }
        return r;
    }

    // 真正 spawn — Stopped 路径
    slot->state = DaemonState::Starting;
    slot->error.clear();
    sl.unlock();
    // 解锁 spawn — daemon 启动可能跑数百 ms,持锁会阻塞 lookup / snapshot 这种
    // 只读调用。等结果回来再上锁写状态 + cv.notify_all。

    int port = pick_free_loopback_port();
    std::string token = make_auth_token();

    bool ok = false;
    std::string err;
    if (port == 0) {
        err = "pick_free_loopback_port returned 0";
    } else if (token.empty()) {
        err = "make_auth_token returned empty";
    } else {
        SpawnRequest sreq;
        sreq.daemon_exe_path = req.daemon_exe_path;
        sreq.port = port;
        sreq.token = token;
        sreq.dangerous = req.dangerous;
        sreq.cwd = req.cwd;            // CreateProcess 用 lpCurrentDirectory 直接落地
        sreq.static_dir = req.static_dir;
        sreq.run_dir = req.run_dir;
        sreq.native_folder_picker_enabled = req.native_folder_picker_enabled;
        auto sr = slot->sup->spawn(sreq);
        if (!sr.ok) {
            err = sr.error;
        } else {
            ok = slot->sup->wait_until_ready(port, wait_timeout);
            bool used_slow_startup_fallback = false;
            if (!ok && slot->sup->running()) {
                auto extra_wait = slow_startup_extra_wait(wait_timeout);
                if (extra_wait.count() > 0) {
                    used_slow_startup_fallback = true;
                    LOG_WARN("[daemon_pool] daemon still running after " +
                             ms_count(wait_timeout) +
                             "ms; extending ready wait by " +
                             ms_count(extra_wait) + "ms for hash=" + req.hash +
                             " context=" + context_id);
                    ok = slot->sup->wait_until_ready(port, extra_wait);
                }
            }
            if (!ok) {
                err = used_slow_startup_fallback
                    ? "wait_until_ready timed out after slow-start fallback / daemon exited early"
                    : "wait_until_ready timed out / daemon exited early";
            }
        }
    }

    // POSIX/macOS: daemon 没有类似 Windows Job Object 的父死子死机制。
    // 若 desktop 上次正常退出但 daemon 以孤儿进程形式继续运行,下次 spawn
    // 新 daemon 时会因"already running"而立刻退出。
    // 处理:spawn 失败后检查 run_dir 内的 port/token 文件,若原有 daemon 仍
    // 在监听则直接复用(adopt),跳过重新 spawn。
    if (!ok && !req.run_dir.empty()) {
        acecode::daemon::RuntimeValidationOptions options;
        options.heartbeat_timeout_ms = acecode::constants::DEFAULT_HEARTBEAT_TIMEOUT_MS;
        auto snapshot = acecode::daemon::read_runtime_snapshot(req.run_dir);
        auto reuse = acecode::daemon::validate_runtime_snapshot_for_reuse(snapshot, options);
        if (reuse.reusable && snapshot.port.has_value() && snapshot.token.has_value()) {
            port = *snapshot.port;
            token = *snapshot.token;
            ok = true;
            err.clear();
            LOG_INFO("[daemon_pool] reclaimed existing daemon at port=" +
                     std::to_string(port) + " for hash=" + req.hash);
        } else {
            LOG_WARN("[daemon_pool] existing daemon runtime not reusable for hash=" +
                     req.hash + " context=" + context_id + ": " + reuse.reason);
        }
    }

    sl.lock();
    if (ok) {
        slot->port = port;
        slot->token = token;
        slot->state = DaemonState::Running;
        slot->cv.notify_all();
        r.ok = true;
        r.port = port;
        r.token = token;
    } else {
        slot->error = err;
        slot->state = DaemonState::Failed;
        slot->cv.notify_all();
        // 失败的 supervisor 可能持有半启动的子进程 — 显式 stop 释放 Job。
        if (slot->sup) slot->sup->stop();
        r.error = err;
        LOG_WARN("[daemon_pool] spawn failed for hash=" + req.hash +
                 " context=" + context_id + ": " + err);
    }
    return r;
}

DaemonPool::Snapshot DaemonPool::lookup(const std::string& hash,
                                        const std::string& context_id) const {
    std::lock_guard<std::mutex> lk(main_mu_);
    auto it = slots_.find(slot_key(hash, context_id));
    if (it == slots_.end()) return {};
    Slot* s = it->second.get();
    std::lock_guard<std::mutex> sl(s->mu);
    Snapshot snap;
    snap.state = s->state;
    snap.port = s->port;
    snap.token = s->token;
    snap.error = s->error;
    return snap;
}

std::vector<std::pair<std::string, DaemonPool::Snapshot>> DaemonPool::snapshot_all() const {
    std::vector<std::pair<std::string, Snapshot>> out;
    std::lock_guard<std::mutex> lk(main_mu_);
    out.reserve(slots_.size());
    for (auto& [hash, s] : slots_) {
        std::lock_guard<std::mutex> sl(s->mu);
        Snapshot snap;
        snap.state = s->state;
        snap.port = s->port;
        snap.token = s->token;
        snap.error = s->error;
        out.emplace_back(hash, snap);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> DaemonPool::stop_all() {
    std::vector<std::pair<std::string, std::string>> failures;
    std::lock_guard<std::mutex> lk(main_mu_);
    for (auto& [hash, s] : slots_) {
        try {
            std::lock_guard<std::mutex> sl(s->mu);
            if (s->sup) s->sup->stop();
            s->state = DaemonState::Stopped;
            s->port = 0;
            s->token.clear();
        } catch (const std::exception& e) {
            failures.emplace_back(hash, e.what());
        } catch (...) {
            failures.emplace_back(hash, "unknown exception in stop");
        }
    }
    return failures;
}

void DaemonPool::set_supervisor_factory_for_test(SupervisorFactory factory) {
    std::lock_guard<std::mutex> lk(main_mu_);
    factory_ = std::move(factory);
}

} // namespace acecode::desktop
