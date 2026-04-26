#include "supervisor.hpp"

#include "platform.hpp"
#include "runtime_files.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <sstream>

namespace acecode::daemon {

Supervisor::Supervisor(SupervisorConfig cfg) : cfg_(std::move(cfg)) {}

Supervisor::~Supervisor() {
    stop();
}

void Supervisor::start() {
    if (started_) return;
    started_ = true;
    stop_.store(false);

    launch_new_worker();
    thread_ = std::thread([this] { run_loop(); });
}

void Supervisor::stop() {
    if (!started_) return;
    stop_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();

    std::int64_t to_kill = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        to_kill = worker_pid_;
        worker_pid_ = 0;
    }
    if (to_kill > 0 && is_pid_alive(to_kill)) {
        terminate_pid(to_kill);
    }
    started_ = false;
}

std::int64_t Supervisor::current_worker_pid() const {
    std::lock_guard<std::mutex> lk(mu_);
    return worker_pid_;
}

void Supervisor::launch_new_worker() {
    std::int64_t pid = 0;
    if (cfg_.spawn_worker) pid = cfg_.spawn_worker();

    std::lock_guard<std::mutex> lk(mu_);
    worker_pid_ = pid;
    if (pid <= 0) {
        LOG_ERROR("[supervisor] failed to spawn worker");
    } else {
        std::ostringstream oss;
        oss << "[supervisor] worker spawned pid=" << pid;
        LOG_INFO(oss.str());
    }
}

void Supervisor::run_loop() {
    while (!stop_.load()) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(cfg_.poll_interval_ms),
                     [this] { return stop_.load(); });
        if (stop_.load()) break;
        lk.unlock();

        // 1) 心跳判活: 读文件,看 timestamp_ms 落后多少
        auto hb = read_heartbeat();
        std::int64_t now = now_unix_ms();
        bool stale = !hb.has_value() ||
                     (now - hb->timestamp_ms) > cfg_.timeout_ms;

        // 2) pid 判活
        std::int64_t pid_snapshot;
        {
            std::lock_guard<std::mutex> lk2(mu_);
            pid_snapshot = worker_pid_;
        }
        bool alive = (pid_snapshot > 0) && is_pid_alive(pid_snapshot);

        if (!alive || stale) {
            std::ostringstream oss;
            oss << "[supervisor] worker stale (pid=" << pid_snapshot
                << " alive=" << alive << " stale_hb=" << stale
                << "); restarting";
            LOG_WARN(oss.str());

            if (alive) terminate_pid(pid_snapshot, /*wait_ms=*/3000);
            launch_new_worker();
        }
    }
}

} // namespace acecode::daemon
