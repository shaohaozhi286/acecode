#include "daemon_pool.hpp"

#include "../daemon/guid.hpp"
#include "../daemon/platform.hpp"
#include "../daemon/runtime_files.hpp"
#include "daemon_protocol.hpp"
#include "../utils/constants.hpp"
#include "../utils/logger.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <thread>

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

bool has_runtime_bundle(const acecode::daemon::RuntimeSnapshot& snapshot) {
    return snapshot.pid.has_value() || snapshot.port.has_value() ||
           snapshot.guid.has_value() || snapshot.heartbeat.has_value() ||
           snapshot.token.has_value() || snapshot.desktop_managed.has_value();
}

bool has_complete_runtime_bundle(
    const acecode::daemon::RuntimeSnapshot& snapshot) {
    return snapshot.pid.has_value() && snapshot.port.has_value() &&
           snapshot.guid.has_value() && snapshot.heartbeat.has_value() &&
           snapshot.token.has_value();
}

acecode::daemon::RuntimeSnapshot read_settled_runtime_snapshot(
    const std::string& run_dir) {
    auto snapshot = acecode::daemon::read_runtime_snapshot(run_dir);
    if (!has_runtime_bundle(snapshot) || has_complete_runtime_bundle(snapshot)) {
        return snapshot;
    }

    // A surviving daemon can still be between its atomic guid/pid/port/token/
    // manifest/heartbeat writes when Desktop relaunches immediately. Wait for
    // that short publication window before classifying or cleaning anything.
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        snapshot = acecode::daemon::read_runtime_snapshot(run_dir);
        if (!has_runtime_bundle(snapshot) ||
            has_complete_runtime_bundle(snapshot)) {
            break;
        }
    }
    return snapshot;
}

ExistingDaemonProbeResult inspect_existing_daemon(
    const ActivateRequest& req) {
    ExistingDaemonProbeResult result;
    if (!req.desktop_managed || req.run_dir.empty()) return result;

    const auto snapshot = read_settled_runtime_snapshot(req.run_dir);
    if (!has_runtime_bundle(snapshot)) {
        return result;
    }

    if (!snapshot.pid.has_value()) {
        const bool heartbeat_process_alive =
            snapshot.heartbeat.has_value() &&
            acecode::daemon::is_pid_alive(snapshot.heartbeat->pid);
        const bool manifest_process_alive =
            snapshot.desktop_managed.has_value() &&
            acecode::daemon::is_pid_alive(snapshot.desktop_managed->pid);
        const bool recorded_port_reachable =
            snapshot.port.has_value() &&
            acecode::daemon::probe_loopback_port(*snapshot.port);
        if (heartbeat_process_alive || manifest_process_alive ||
            recorded_port_reachable) {
            result.action = ExistingDaemonAction::Unsafe;
            result.reason =
                "incomplete runtime bundle still references a live process";
            return result;
        }
        if (snapshot.guid.has_value()) {
            acecode::daemon::cleanup_runtime_files_if_owned(
                0, *snapshot.guid, req.run_dir, true);
        }
        result.reason = "removed an incomplete stopped runtime generation";
        return result;
    }

    if (!snapshot.guid.has_value()) {
        if (acecode::daemon::is_pid_alive(*snapshot.pid)) {
            result.action = ExistingDaemonAction::Unsafe;
            result.reason =
                "live runtime process is missing its generation GUID";
        } else {
            result.reason = "ignored an incomplete stopped runtime generation";
        }
        return result;
    }

    const bool has_generation =
        snapshot.pid.has_value() && snapshot.guid.has_value();
    const bool manifest_matches =
        has_generation && snapshot.desktop_managed.has_value() &&
        snapshot.desktop_managed->pid == *snapshot.pid &&
        snapshot.desktop_managed->guid == *snapshot.guid &&
        snapshot.desktop_managed->kind == kDesktopManagedRuntimeKind;
    const bool recorded_pid_alive =
        acecode::daemon::is_pid_alive(*snapshot.pid);
    const auto process_identity = recorded_pid_alive
        ? acecode::daemon::inspect_daemon_process_identity(*snapshot.pid)
        : acecode::daemon::DaemonProcessIdentity::Unknown;
    const auto process_start_time = recorded_pid_alive
        ? acecode::daemon::process_start_time_ms(*snapshot.pid)
        : std::optional<std::int64_t>{};

    acecode::daemon::RuntimeValidationOptions options;
    options.heartbeat_timeout_ms =
        acecode::constants::DEFAULT_HEARTBEAT_TIMEOUT_MS;
    options.require_process_identity = true;
    const auto reuse =
        acecode::daemon::validate_runtime_snapshot_for_reuse(snapshot, options);

    if (!reuse.reusable) {
        if (has_generation && !recorded_pid_alive) {
            acecode::daemon::cleanup_runtime_files_if_owned(
                *snapshot.pid, *snapshot.guid, req.run_dir, true);
            result.reason = reuse.reason;
            return result;
        }
        if (manifest_matches &&
            acecode::daemon::runtime_pid_reuse_is_proven(
                snapshot, process_identity, process_start_time)) {
            if (acecode::daemon::cleanup_runtime_files_if_owned(
                    *snapshot.pid, *snapshot.guid, req.run_dir, true)) {
                LOG_INFO(
                    "[daemon_pool] removed stale managed runtime after PID reuse pid=" +
                    std::to_string(*snapshot.pid));
                result.reason =
                    "removed stale Desktop-managed runtime after PID reuse";
                return result;
            }
            result.action = ExistingDaemonAction::Unsafe;
            result.reason =
                "runtime generation changed while discarding reused PID state";
            return result;
        }
        if (manifest_matches &&
            process_identity == acecode::daemon::DaemonProcessIdentity::Match) {
            result.action = ExistingDaemonAction::Replace;
            result.pid = *snapshot.pid;
            result.guid = *snapshot.guid;
            result.reason = "verified Desktop-managed daemon is unhealthy: " +
                            reuse.reason;
            return result;
        }
        result.action = ExistingDaemonAction::Unsafe;
        result.reason =
            "existing runtime process is not safe to replace: " + reuse.reason;
        return result;
    }

    result.pid = *snapshot.pid;
    result.port = *snapshot.port;
    result.token = snapshot.token.value_or(std::string{});
    result.guid = snapshot.guid.value_or(std::string{});

    nlohmann::json health;
    try {
        const auto response = cpr::Get(
            cpr::Url{"http://127.0.0.1:" + std::to_string(result.port) +
                     "/api/health"},
            cpr::Header{{"X-ACECode-Token", result.token}},
            cpr::Proxies{{"http", ""}, {"https", ""}},
            cpr::Timeout{1200});
        if (response.status_code != 200) {
            result.action = manifest_matches
                ? ExistingDaemonAction::Replace
                : ExistingDaemonAction::Unsafe;
            result.reason = manifest_matches
                ? "verified Desktop-managed daemon health failed"
                : "existing daemon health is unavailable";
            return result;
        }
        health = nlohmann::json::parse(response.text);
    } catch (const std::exception& e) {
        result.action = manifest_matches
            ? ExistingDaemonAction::Replace
            : ExistingDaemonAction::Unsafe;
        result.reason = manifest_matches
            ? std::string("verified Desktop-managed daemon health parse failed: ") +
                  e.what()
            : "existing daemon identity could not be verified";
        return result;
    }

    const bool health_identity_matches =
        health.value("pid", static_cast<std::int64_t>(0)) == result.pid &&
        health.value("guid", std::string{}) == result.guid &&
        health.value("port", 0) == result.port;
    if (!health_identity_matches) {
        result.action = ExistingDaemonAction::Unsafe;
        result.reason = "runtime and live health identity disagree";
        return result;
    }

    const bool health_managed = health.value("desktop_managed", false);
    const int protocol = health.value("desktop_protocol_version", 0);
    if (manifest_matches && health_managed) {
        if (snapshot.desktop_managed->protocol_version != protocol) {
            result.action = ExistingDaemonAction::Unsafe;
            result.reason =
                "runtime manifest and live health protocol disagree";
            return result;
        }
        result.action = protocol == kDesktopDaemonProtocolVersion
            ? ExistingDaemonAction::Reuse
            : ExistingDaemonAction::Replace;
        result.reason = result.action == ExistingDaemonAction::Reuse
            ? "healthy compatible Desktop-managed daemon"
            : "Desktop daemon protocol is incompatible";
        return result;
    }

    // One-release migration for the pre-manifest Desktop daemon.
    const bool legacy_reserved_dir =
        std::filesystem::path(req.run_dir).filename() == "desktop-shared";
    if (!snapshot.desktop_managed.has_value() && !health_managed &&
        legacy_reserved_dir &&
        acecode::daemon::process_is_acecode_daemon(result.pid)) {
        result.action = ExistingDaemonAction::Replace;
        result.reason = "legacy Desktop-managed daemon requires protocol upgrade";
        return result;
    }

    result.action = ExistingDaemonAction::Unsafe;
    result.reason = "existing daemon is not Desktop-managed";
    return result;
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
    slot->sup->set_keep_alive_on_exit(keep_alive_on_exit_);
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

    bool ok = false;
    std::string err;
    int port = 0;
    std::string token;
    DaemonConnectionSource source = DaemonConnectionSource::None;

    ExistingDaemonProbe probe;
    {
        std::lock_guard<std::mutex> lk(main_mu_);
        probe = existing_daemon_probe_;
    }
    const auto existing = probe ? probe(req) : inspect_existing_daemon(req);

    if (existing.action == ExistingDaemonAction::Unsafe) {
        err = existing.reason.empty()
            ? "existing daemon identity is unsafe"
            : existing.reason;
    } else if (existing.action == ExistingDaemonAction::Reuse) {
        auto attached = slot->sup->attach(existing.pid);
        if (!attached.ok) {
            err = "attach existing daemon failed: " + attached.error;
        } else {
            port = existing.port;
            token = existing.token;
            source = DaemonConnectionSource::Attached;
            ok = true;
            LOG_INFO("[daemon_pool] attached compatible daemon pid=" +
                     std::to_string(existing.pid) + " port=" +
                     std::to_string(port) + " for hash=" + req.hash);
        }
    } else {
        if (existing.action == ExistingDaemonAction::Replace) {
            auto attached = slot->sup->attach(existing.pid);
            if (!attached.ok) {
                err = "attach incompatible daemon for replacement failed: " +
                      attached.error;
            } else {
                LOG_INFO("[daemon_pool] replacing managed daemon pid=" +
                         std::to_string(existing.pid) + ": " + existing.reason);
                slot->sup->stop();
                if (acecode::daemon::is_pid_alive(existing.pid)) {
                    err =
                        "verified Desktop-managed daemon did not exit during "
                        "replacement";
                } else {
                    acecode::daemon::cleanup_runtime_files_if_owned(
                        existing.pid, existing.guid, req.run_dir, true);
                }
            }
        }
        if (err.empty()) {
            port = pick_free_loopback_port();
            token = make_auth_token();
        }
        if (err.empty() && port == 0) {
            err = "pick_free_loopback_port returned 0";
        } else if (err.empty() && token.empty()) {
            err = "make_auth_token returned empty";
        }
    }

    if (!ok && err.empty()) {
        SpawnRequest sreq;
        sreq.daemon_exe_path = req.daemon_exe_path;
        sreq.port = port;
        sreq.token = token;
        sreq.dangerous = req.dangerous;
        sreq.cwd = req.cwd;            // CreateProcess 用 lpCurrentDirectory 直接落地
        sreq.static_dir = req.static_dir;
        sreq.run_dir = req.run_dir;
        sreq.native_folder_picker_enabled = req.native_folder_picker_enabled;
        sreq.desktop_managed = req.desktop_managed;
        sreq.guid = req.desktop_managed
            ? acecode::daemon::generate_daemon_guid()
            : std::string{};
        sreq.desktop_protocol_version = req.desktop_managed
            ? kDesktopDaemonProtocolVersion
            : 0;
        sreq.desktop_owner_pid = req.desktop_owner_pid;
        sreq.desktop_owner_instance = req.desktop_owner_instance;
        auto sr = slot->sup->spawn(sreq);
        if (!sr.ok) {
            err = sr.error;
        } else {
            ok = slot->sup->wait_until_ready(port, wait_timeout);
            if (ok) source = DaemonConnectionSource::Spawned;
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
                    if (ok) source = DaemonConnectionSource::Spawned;
                }
            }
            if (!ok) {
                err = used_slow_startup_fallback
                    ? "wait_until_ready timed out after slow-start fallback / daemon exited early"
                    : "wait_until_ready timed out / daemon exited early";
            }
        }
    }

    sl.lock();
    if (ok) {
        slot->port = port;
        slot->token = token;
        slot->state = DaemonState::Running;
        slot->source = source;
        slot->cv.notify_all();
        r.ok = true;
        r.port = port;
        r.token = token;
    } else {
        slot->error = err;
        slot->state = DaemonState::Failed;
        slot->source = DaemonConnectionSource::None;
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
    snap.source = s->source;
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
        snap.source = s->source;
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
            s->source = DaemonConnectionSource::None;
        } catch (const std::exception& e) {
            failures.emplace_back(hash, e.what());
        } catch (...) {
            failures.emplace_back(hash, "unknown exception in stop");
        }
    }
    return failures;
}

std::vector<std::pair<std::string, std::string>> DaemonPool::shutdown_all() {
    std::vector<std::pair<std::string, std::string>> failures;
    std::lock_guard<std::mutex> lk(main_mu_);
    for (auto& [hash, s] : slots_) {
        try {
            std::lock_guard<std::mutex> sl(s->mu);
            if (s->sup) {
                if (keep_alive_on_exit_) s->sup->release();
                else s->sup->stop();
            }
            s->state = DaemonState::Stopped;
            s->port = 0;
            s->token.clear();
            s->source = DaemonConnectionSource::None;
        } catch (const std::exception& e) {
            failures.emplace_back(hash, e.what());
        } catch (...) {
            failures.emplace_back(hash, "unknown exception in shutdown");
        }
    }
    return failures;
}

void DaemonPool::set_keep_alive_on_exit(bool keep_alive) {
    std::lock_guard<std::mutex> lk(main_mu_);
    keep_alive_on_exit_ = keep_alive;
    for (auto& [key, slot] : slots_) {
        (void)key;
        std::lock_guard<std::mutex> sl(slot->mu);
        if (slot->sup) slot->sup->set_keep_alive_on_exit(keep_alive);
    }
}

bool DaemonPool::keep_alive_on_exit() const {
    std::lock_guard<std::mutex> lk(main_mu_);
    return keep_alive_on_exit_;
}

void DaemonPool::set_supervisor_factory_for_test(SupervisorFactory factory) {
    std::lock_guard<std::mutex> lk(main_mu_);
    factory_ = std::move(factory);
}

void DaemonPool::set_existing_daemon_probe_for_test(
    ExistingDaemonProbe probe) {
    std::lock_guard<std::mutex> lk(main_mu_);
    existing_daemon_probe_ = std::move(probe);
}

} // namespace acecode::desktop
