#pragma once

// desktop shell 的 daemon 子进程管控者: 起一个 acecode.exe daemon --foreground
// 子进程,绑到 Job Object 让父进程退出时子进程一起死,等到子进程的 HTTP 端口
// 真正能 connect 后再回调上层。
//
// 设计原因:
//   - WebView 进程加载 http://127.0.0.1:<port>/?token=... 之前必须确认 daemon
//     已 listening,否则会显示 "无法连接到此网站",用户体验很差。
//   - 父进程崩溃 (WebView 越界 / 调试器 abort) 不应留下野生 daemon。Windows
//     用 Job Object 的 KILL_ON_JOB_CLOSE 是最简方案。
//   - port + token 都由父进程预生成传入子进程: 避免父子两端跨进程读 run/ 文件
//     存在的时序竞争(daemon 写文件 vs 父进程 poll)。
//
// v1 仅实现 Windows 路径。POSIX 版本是后续的扩展点(fork+execve+prctl PDEATHSIG)。

#include <chrono>
#include <string>

namespace acecode::desktop {

struct SpawnRequest {
    std::string daemon_exe_path;   // acecode.exe 全路径(不能为空)
    int         port;              // 父进程预选的空闲 loopback 端口(>0)
    std::string token;             // 父进程预生成的 auth token(非空)
    bool        dangerous = false; // 透传 -dangerous(MVP 阶段一般不开)
    std::string cwd;               // 子进程启动时的 current_path。空 = 继承父进程当前目录
    std::string static_dir;        // 非空时通过 --static-dir=<path> 注入,daemon 走 FileSystem 资源(dev 模式)
};

struct SpawnResult {
    bool        ok = false;       // 整体成功标记
    std::string error;            // ok=false 时的人类可读原因
    long long   pid = 0;          // 子进程 pid(成功时填)
};

// 抽象接口: 让 DaemonPool 单测能注入 mock supervisor。生产代码用具体类
// DaemonSupervisor(下面定义),它实现这个接口。
class IDaemonSupervisor {
public:
    virtual ~IDaemonSupervisor() = default;
    virtual SpawnResult spawn(const SpawnRequest& req) = 0;
    virtual bool wait_until_ready(int port, std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
};

class DaemonSupervisor : public IDaemonSupervisor {
public:
    DaemonSupervisor();
    ~DaemonSupervisor() override;

    DaemonSupervisor(const DaemonSupervisor&) = delete;
    DaemonSupervisor& operator=(const DaemonSupervisor&) = delete;

    // 起子进程并把 stdout/stderr 重定向到 nul(MVP 不读输出 — 父子之间靠
    // port+token 已对齐,不需要 daemon 喷 JSON line)。
    SpawnResult spawn(const SpawnRequest& req) override;

    // 阻塞等子进程的 TCP 端口可 connect。timeout 过期返回 false。
    // 实现: 每 100ms TCP connect 一次,首个成功即返回。
    bool wait_until_ready(int port, std::chrono::milliseconds timeout) override;

    // 优雅停: Windows 下对 Job 调 TerminateJobObject(干脆,因为
    // GenerateConsoleCtrlEvent 跨进程到 detached child 不可靠)。
    void stop() override;

    bool running() const override;

private:
    struct Impl;
    Impl* impl_;
};

// 工具: 选一个 loopback 空闲端口。失败返回 0。Windows 走 winsock,
// 用 ::bind(:0) + getsockname 拿系统分配的端口,然后立刻 closesocket。
// 注意: TOCTOU — 拿到端口和 daemon bind 之间窗口期里端口可能被别人抢,
// MVP 接受这个小概率失败(失败时整个 spawn 流程会因 daemon bind 报错退出)。
int pick_free_loopback_port();

// 工具: 生成 32 字节 hex token,内部委托给 acecode::generate_auth_token。
// 单独包一层是为了让 daemon_supervisor.cpp 不直接 include utils/token.hpp,
// 减小 include 面 — 同时方便测试时 stub。
std::string make_auth_token();

} // namespace acecode::desktop
