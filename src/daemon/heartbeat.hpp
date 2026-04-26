#pragma once

// HeartbeatWriter: worker 进程内启动一个独立线程,按 cfg.daemon.heartbeat_interval_ms
// 节奏刷新 ~/.acecode/run/heartbeat。文件内容是 JSON {pid, guid, timestamp_ms}
// —— supervisor 直接读 timestamp_ms 判超时,不依赖文件 mtime(FAT32 / 网络盘
// mtime 精度 1s 起,2s 心跳容易误判)。
//
// 生命周期: 构造 → start() 起线程 → 析构 / stop() 通知线程退出。
// 线程退出 = condvar 唤醒 + 退出循环,不依赖 OS 信号。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace acecode::daemon {

class HeartbeatWriter {
public:
    HeartbeatWriter(std::int64_t pid, std::string guid, int interval_ms);
    ~HeartbeatWriter();

    HeartbeatWriter(const HeartbeatWriter&) = delete;
    HeartbeatWriter& operator=(const HeartbeatWriter&) = delete;

    // 起后台线程。重复调用是 no-op。
    void start();

    // 通知后台线程退出并 join。析构会自动调用,但显式 stop 用于确定性关闭。
    void stop();

private:
    void run_loop();

    std::int64_t pid_;
    std::string  guid_;
    int          interval_ms_;

    std::thread             thread_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::atomic<bool>       stop_{false};
    bool                    started_ = false;
};

} // namespace acecode::daemon
