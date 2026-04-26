#pragma once

// Supervisor: 仅 launcher 进程使用(Windows Service 模式下 SCM 启动的进程)。
// 周期性读 ~/.acecode/run/heartbeat,若 timestamp_ms 落后超过
// cfg.daemon.heartbeat_timeout_ms,认定 worker 僵死 → terminate_pid + 重启。
//
// v1 单 worker 监督,不支持多 worker。设计上保留可扩展性: launcher 持有一个
// guid,worker 启动时通过 --guid 校验 guid 一致才启动,避免上次崩溃留下的
// 野 worker 与新 launcher 共存。
//
// 这个类的 start() 会起一个独立线程,所以析构 / stop() 必须先于 launcher
// 进程退出被调用,否则线程访问已销毁的成员。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acecode::daemon {

// SpawnFn 由调用方提供: 启动一个新的 worker 进程,返回 pid(0 表示失败)。
// 通常实现是 spawn_detached({exe_path, "daemon", "--supervised", "--guid=<G>"})。
using SpawnFn = std::function<std::int64_t()>;

struct SupervisorConfig {
    std::string  guid;                     // launcher 派给 worker 的 GUID
    int          poll_interval_ms = 5000;  // 每 5s 看一次
    std::int64_t timeout_ms       = 15000; // 心跳落后多少 ms 视为僵死
    SpawnFn      spawn_worker;             // 必填: 起 worker
};

class Supervisor {
public:
    explicit Supervisor(SupervisorConfig cfg);
    ~Supervisor();

    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    // 起后台线程开始监督。立刻启一次 worker(如果没在跑)。
    void start();

    // 通知后台线程退出 + 终止当前 worker(如果还在跑)+ join。
    void stop();

    // 当前监督的 worker pid(可能为 0,表示尚未启动 / 已死)。
    std::int64_t current_worker_pid() const;

private:
    void run_loop();
    void launch_new_worker();

    SupervisorConfig cfg_;
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::thread             thread_;
    std::atomic<bool>       stop_{false};
    bool                    started_ = false;
    std::int64_t            worker_pid_ = 0;
};

} // namespace acecode::daemon
