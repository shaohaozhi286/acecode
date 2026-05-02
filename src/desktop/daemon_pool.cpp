#include "daemon_pool.hpp"

#include "../utils/logger.hpp"

namespace acecode::desktop {

DaemonPool::~DaemonPool() {
    // 不主动 stop_all — 让调用方控制(quit 时显式调,析构时机可能在异常路径上)。
}

DaemonPool::Slot* DaemonPool::get_or_create_slot(const std::string& hash) {
    std::lock_guard<std::mutex> lk(main_mu_);
    auto it = slots_.find(hash);
    if (it != slots_.end()) return it->second.get();
    auto slot = std::make_unique<Slot>();
    if (factory_) {
        slot->sup = factory_();
    } else {
        slot->sup = std::make_unique<DaemonSupervisor>();
    }
    Slot* raw = slot.get();
    slots_.emplace(hash, std::move(slot));
    return raw;
}

ActivateResult DaemonPool::activate(const ActivateRequest& req,
                                    std::chrono::milliseconds wait_timeout) {
    ActivateResult r;
    if (req.hash.empty() || req.cwd.empty() || req.daemon_exe_path.empty()) {
        r.error = "activate: missing hash/cwd/daemon_exe_path";
        return r;
    }

    Slot* slot = get_or_create_slot(req.hash);

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
        auto sr = slot->sup->spawn(sreq);
        if (!sr.ok) {
            err = sr.error;
        } else {
            ok = slot->sup->wait_until_ready(port, wait_timeout);
            if (!ok) err = "wait_until_ready timed out / daemon exited early";
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
        LOG_WARN("[daemon_pool] spawn failed for hash=" + req.hash + ": " + err);
    }
    return r;
}

DaemonPool::Snapshot DaemonPool::lookup(const std::string& hash) const {
    std::lock_guard<std::mutex> lk(main_mu_);
    auto it = slots_.find(hash);
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
