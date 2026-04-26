#include "heartbeat.hpp"

#include "runtime_files.hpp"
#include "../utils/logger.hpp"

#include <chrono>

namespace acecode::daemon {

HeartbeatWriter::HeartbeatWriter(std::int64_t pid, std::string guid, int interval_ms)
    : pid_(pid), guid_(std::move(guid)), interval_ms_(interval_ms) {}

HeartbeatWriter::~HeartbeatWriter() {
    stop();
}

void HeartbeatWriter::start() {
    if (started_) return;
    started_ = true;
    stop_.store(false);

    // 启动前先写一次,让 supervisor 立刻能看到第一帧。
    Heartbeat hb{pid_, guid_, now_unix_ms()};
    write_heartbeat(hb);

    thread_ = std::thread([this] { run_loop(); });
}

void HeartbeatWriter::stop() {
    if (!started_) return;
    stop_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

void HeartbeatWriter::run_loop() {
    while (!stop_.load()) {
        // 用 condvar wait_for,这样 stop() 能即时打断,不必等满 interval_ms。
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(interval_ms_),
                     [this] { return stop_.load(); });
        if (stop_.load()) break;

        Heartbeat hb{pid_, guid_, now_unix_ms()};
        if (!write_heartbeat(hb)) {
            LOG_WARN("[daemon] heartbeat write failed");
        }
    }
}

} // namespace acecode::daemon
