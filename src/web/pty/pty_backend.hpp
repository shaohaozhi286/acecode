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
#include <optional>
#include <string>
#include <vector>

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
// 否则 Windows 取 %COMSPEC%(空则 "cmd.exe"),POSIX 取 $SHELL → passwd 登录
// shell(getpwuid,GUI 启动的 daemon 没有 $SHELL)→ "/bin/sh"。
std::string resolve_console_shell(const std::string& configured);

// ── 控制台 shell 目录(plan: 控制台 Shell 选择器)──────────────────────
// 探测当前 OS 可选的 shell(Windows: PowerShell / Git Bash / cmd;POSIX:
// 默认 $SHELL / bash / zsh / fish),供前端 + 旁下拉框选择。command 为实际
// 启动命令行(可带参数,如 git bash 的 "--login -i");available=false 表示
// 该 shell 不可用,git-bash 探测不到时 needs_path=true(前端提示用户指定
// bash.exe 路径)。
struct ConsoleShellOption {
    std::string id;        // 稳定 id:powershell / git-bash / cmd / shell / bash / zsh / fish
    std::string label;     // 展示名
    std::string command;   // 启动命令行(available=false 时可空)
    bool available = false;
    bool needs_path = false;
};

// 注入式探测点(默认实现走真实 FS / env / 注册表),便于单测 mock。
struct ShellProbe {
    std::function<bool(const std::string& path)> exists;        // 文件存在?
    std::function<std::string(const std::string& name)> getenv; // 环境变量(UTF-8),无则空
    std::function<std::string()> git_install_path;              // Windows: GitForWindows 注册表 InstallPath,无则空
    std::function<std::string()> login_shell;                   // POSIX: passwd 登录 shell(getpwuid),无则空;mock 可不设
};
ShellProbe default_shell_probe();

// configured_git_bash_path = config.console.git_bash_path(用户指定的 bash.exe,可空)。
std::vector<ConsoleShellOption> detect_console_shells(
    const std::string& configured_git_bash_path, const ShellProbe& probe);
std::vector<ConsoleShellOption> detect_console_shells(
    const std::string& configured_git_bash_path);  // 用 default_shell_probe()

// 按 id 解析启动命令;不存在 / 不可用返回 nullopt。
std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const std::string& configured_git_bash_path,
    const ShellProbe& probe);
std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const std::string& configured_git_bash_path);

// 默认 shell id:configured_default_shell 非空且可用则用之,否则平台默认
// (Windows="cmd",POSIX="shell")。
std::string default_console_shell_id(
    const std::string& configured_default_shell,
    const std::string& configured_git_bash_path, const ShellProbe& probe);
std::string default_console_shell_id(
    const std::string& configured_default_shell,
    const std::string& configured_git_bash_path);

// bash 路径是否为 WSL 的 System32\bash.exe(需排除,它不是 Git Bash)。
bool is_wsl_system32_bash(const std::string& path);

} // namespace acecode
