#pragma once

// 多 workspace 模型下,desktop 端的 daemon 进程池。每个 cwd_hash 对应一个独立
// daemon 子进程(独立端口 + 独立 token + 独立 Job Object)。
//
// 设计原则:
//   - lazy spawn: 仅在 activate(hash) 被调用时才 spawn 该 workspace 的 daemon。
//   - per-key serialization: 同一 hash 并发 activate 串行化(条件变量等就绪),
//     不同 hash 并发执行不互相阻塞。
//   - daemon 自身代码零改动:每个 daemon 仍只服务一个 cwd(自己进程的 current_path)。
//   - stop_all best-effort: 一个 daemon 停失败不阻塞其余。
//
// 线程安全:外部所有方法可任意线程并发调用。内部 main_mu_ 保护 slots_;每个 Slot
// 自带 cv_ 让等待者醒来。

#include "daemon_supervisor.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace acecode::desktop {

enum class DaemonState {
    Stopped,    // 还没起过 / 已显式 stop 过
    Starting,   // spawn 中,wait_until_ready 未完成
    Running,    // 端口可 connect
    Failed,     // spawn 或 wait_until_ready 失败
};

struct ActivateRequest {
    std::string hash;             // cwd_hash
    std::string context_id = "default"; // 同一 cwd 下的 daemon 上下文(default / resume-...)
    std::string cwd;              // 启 daemon 时设给子进程的 current_path
    std::string daemon_exe_path;  // 同目录的 acecode.exe(由调用方解析)
    bool        dangerous = false;
    std::string static_dir;       // 非空 → daemon 走 FileSystem 静态资源(dev 热重载)
    std::string run_dir;          // 非空 → daemon 把 runtime files 写到这里(per-workspace 隔离 GUID 锁 / heartbeat / port / token)
};

struct ActivateResult {
    bool        ok = false;
    std::string error;
    int         port = 0;
    std::string token;
};

class DaemonPool {
public:
    // 构造时不做 winsock 等初始化(由 DaemonSupervisor 自带)。
    DaemonPool() = default;
    ~DaemonPool();

    DaemonPool(const DaemonPool&) = delete;
    DaemonPool& operator=(const DaemonPool&) = delete;

    // 阻塞直到目标 daemon Running 或失败。
    // 同 hash 并发:串行化 — 第一个 caller 实际 spawn,其余等待返回相同结果。
    ActivateResult activate(const ActivateRequest& req,
                            std::chrono::milliseconds wait_timeout = std::chrono::milliseconds(10000));

    // 查现存 slot;不存在或未 Running 返回 nullopt。
    struct Snapshot {
        DaemonState state = DaemonState::Stopped;
        int port = 0;
        std::string token;
        std::string error; // state=Failed 时填
    };
    Snapshot lookup(const std::string& hash,
                    const std::string& context_id = "default") const;

    // 列出所有 slot 状态 — 给前端 listWorkspaces 用。
    std::vector<std::pair<std::string, Snapshot>> snapshot_all() const;

    // 关闭所有 slot 的 supervisor。返回失败列表(hash + 错误描述);best-effort,
    // 一个 stop 异常不阻塞下一个。
    std::vector<std::pair<std::string, std::string>> stop_all();

    // 测试 hook: 注入 supervisor 工厂(默认是直接 new DaemonSupervisor)。
    using SupervisorFactory = std::function<std::unique_ptr<IDaemonSupervisor>()>;
    void set_supervisor_factory_for_test(SupervisorFactory factory);

private:
    struct Slot {
        // Slot 自身一旦插入 map 就不再移动(unique_ptr 包住),所以这些成员可
        // 被多个等待者并发等。
        std::unique_ptr<IDaemonSupervisor> sup;
        DaemonState state = DaemonState::Stopped;
        int port = 0;
        std::string token;
        std::string error;
        std::mutex mu;
        std::condition_variable cv;
    };

    static std::string slot_key(const std::string& hash, const std::string& context_id);
    Slot* get_or_create_slot(const std::string& hash, const std::string& context_id);

    mutable std::mutex main_mu_;
    std::unordered_map<std::string, std::unique_ptr<Slot>> slots_;
    SupervisorFactory factory_;
};

} // namespace acecode::desktop
