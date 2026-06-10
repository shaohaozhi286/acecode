#pragma once

// 控制台 PTY 后端抽象(openspec/changes/add-console-dock,design.md D3)。
//
// 一个 PtyProcess = 一个挂在伪终端上的 shell 进程。实现内部自带读线程,
// 输出字节经 on_data 回调(读线程上下文)推给上层;进程退出经 on_exit 通知
// (恰好一次,且在最后一段输出之后)。xterm.js 只消费 VT 字节流,后端是
// ConPTY / winpty / 管道 / forkpty 对前端完全透明。
//
// 平台分层:
//   Windows : ConPty(1809+ 首选) → Winpty(< 1809 完整体验) → Pipe(兜底,
//             无 TTY 语义,交互式程序不可用)
//   POSIX   : PosixPty(forkpty,无 fallback 链)
//
// 实现体在 pty_backend_win.cpp / pty_backend_posix.cpp(文件内 #ifdef 包体,
// 与 proxy_resolver 的平台拆分惯例一致)。

#include <functional>
#include <memory>
#include <string>

namespace acecode {

enum class PtyBackendKind {
    ConPty,
    Winpty,
    Pipe,
    PosixPty,
};

// 稳定的协议字符串("conpty" / "winpty" / "pipe" / "posix"),用于
// /api/health console capability 与 PTY session info。
const char* pty_backend_kind_name(PtyBackendKind kind);

struct PtySpawnSpec {
    std::string shell;  // 要启动的 shell 命令(resolve_console_shell 的结果)
    std::string cwd;    // 工作目录(UTF-8)
    int cols = 80;
    int rows = 25;
};

struct PtyCallbacks {
    // PTY 输出字节(读线程上下文调用;实现保证 on_exit 之后不再触发)。
    std::function<void(const std::string& data)> on_data;
    // 进程退出,恰好一次,在最后一段 on_data 之后。
    std::function<void(int exit_code)> on_exit;
};

class PtyProcess {
public:
    virtual ~PtyProcess() = default;

    // 把字节原样写进 PTY 输入(键盘流)。进程已退出时为 no-op。
    virtual void write(const std::string& data) = 0;

    // 调整终端尺寸。Pipe 后端为 no-op。
    virtual void resize(int cols, int rows) = 0;

    // 强制终止进程并回收读线程。幂等;返回后不再有任何回调触发。
    virtual void kill() = 0;

    virtual int pid() const = 0;
    virtual PtyBackendKind kind() const = 0;
};

// 启动期探测一次:当前平台最优可用后端。
PtyBackendKind detect_pty_backend();

// 用指定后端 spawn 一个 shell。失败返回 nullptr 并写 error(UTF-8 诊断文本)。
// callbacks 的 on_data/on_exit 必须可调用;调用方保证回调捕获物寿命覆盖
// PtyProcess 生命周期(kill()/析构返回后才可释放)。
std::unique_ptr<PtyProcess> spawn_pty(PtyBackendKind kind,
                                      const PtySpawnSpec& spec,
                                      PtyCallbacks callbacks,
                                      std::string& error);

// shell 解析(纯函数,单测覆盖):configured 非空(config console.shell)优先;
// 否则 Windows 取 %COMSPEC%(空则 "cmd.exe"),POSIX 取 $SHELL(空则 "/bin/sh")。
std::string resolve_console_shell(const std::string& configured);

} // namespace acecode
