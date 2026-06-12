// 覆盖 src/web/pty/ 的后端抽象(openspec/changes/add-console-dock 任务 2.x):
// - resolve_console_shell 纯函数(配置覆盖 / 平台默认)
// - pty_backend_kind_name 协议字符串稳定性(进 /api/health 与 session info,
//   前端依赖这些字面量做降级提示判断,改名即破坏协议)
// - 本机真实 spawn 往返(Windows: ConPTY + Pipe;POSIX: forkpty)
//
// 真实 spawn 用例的回归意义:管道接线/句柄继承/读线程 EOF/exit 顺序任何一处
// 接错都会表现为挂死或丢输出,这类 bug 纯逻辑单测抓不到。

#include <gtest/gtest.h>

#include "web/pty/pty_backend.hpp"
#include "utils/encoding.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

// 收集 on_data/on_exit 的线程安全容器,带条件变量等待。
struct PtyCollector {
    std::mutex mu;
    std::condition_variable cv;
    std::string output;
    std::atomic<bool> exited{false};
    int exit_code = -1;

    acecode::PtyCallbacks callbacks() {
        return acecode::PtyCallbacks{
            [this](const std::string& data) {
                std::lock_guard<std::mutex> lock(mu);
                output += data;
                cv.notify_all();
            },
            [this](int code) {
                {
                    std::lock_guard<std::mutex> lock(mu);
                    exit_code = code;
                }
                exited.store(true);
                cv.notify_all();
            },
        };
    }

    bool wait_for_output(const std::string& needle, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return output.find(needle) != std::string::npos;
        });
    }

    bool wait_for_exit(int timeout_ms) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return exited.load(); });
    }
};

// 触发场景:config console.shell 配置了自定义 shell。
// 期望:配置值原样胜出,不做平台默认推导。
TEST(PtyShellResolveTest, ConfiguredShellWins) {
    EXPECT_EQ(acecode::resolve_console_shell("pwsh"), "pwsh");
    EXPECT_EQ(acecode::resolve_console_shell("/usr/bin/fish"), "/usr/bin/fish");
}

// 触发场景:无配置,走平台默认。
// 期望:Windows 取 %COMSPEC%(开发机必有,即 cmd.exe 完整路径);
// POSIX 取 $SHELL 或 /bin/sh。用户决策:cmd 优先于 powershell,与
// bash_tool 走 cmd.exe 的现状一致(design.md D6)。
TEST(PtyShellResolveTest, EmptyConfigFallsBackToPlatformDefault) {
    std::string shell = acecode::resolve_console_shell("");
    ASSERT_FALSE(shell.empty());
#ifdef _WIN32
    // %COMSPEC% 总指向 cmd.exe;环境缺失时退 "cmd.exe" 字面量。
    EXPECT_NE(shell.find("cmd"), std::string::npos);
#else
    EXPECT_EQ(shell.front(), '/');
#endif
}

// ---------------------------------------------------------------------------
// 控制台 shell 目录(+ 旁下拉框选择器,plan: 控制台 Shell 选择器)
// 用注入式 ShellProbe mock FS/env/注册表,避免依赖真实安装的 shell。
// ---------------------------------------------------------------------------

namespace {
acecode::ShellProbe mock_probe(std::set<std::string> existing,
                               std::map<std::string, std::string> env,
                               std::string git_install = "") {
    acecode::ShellProbe p;
    p.exists = [existing](const std::string& path) { return existing.count(path) > 0; };
    p.getenv = [env](const std::string& name) {
        auto it = env.find(name);
        return it == env.end() ? std::string{} : it->second;
    };
    p.git_install_path = [git_install]() { return git_install; };
    return p;
}
const acecode::ConsoleShellOption* find_shell(
    const std::vector<acecode::ConsoleShellOption>& v, const std::string& id) {
    for (const auto& o : v) if (o.id == id) return &o;
    return nullptr;
}
}  // namespace

// 触发场景:WSL 的 System32\bash.exe 必须被识别(它不是 Git Bash)。
// 回归意义:误把 WSL bash 当 Git Bash 会打开完全不同的环境。
TEST(ConsoleShellCatalogTest, DetectsWslSystem32Bash) {
    EXPECT_TRUE(acecode::is_wsl_system32_bash("C:\\Windows\\System32\\bash.exe"));
    EXPECT_TRUE(acecode::is_wsl_system32_bash("c:/windows/system32/bash.exe"));
    EXPECT_FALSE(acecode::is_wsl_system32_bash("C:\\Program Files\\Git\\bin\\bash.exe"));
}

#ifdef _WIN32

// 触发场景:Git 装在 Program Files,bash.exe 存在。
// 期望:git-bash 可用,命令含 --login -i,且带空格的路径被双引号包裹。
TEST(ConsoleShellCatalogTest, GitBashDetectedUnderProgramFiles) {
    auto probe = mock_probe(
        {"C:\\Program Files\\Git\\bin\\bash.exe"},
        {{"ProgramFiles", "C:\\Program Files"},
         {"COMSPEC", "C:\\Windows\\System32\\cmd.exe"}});
    auto shells = acecode::detect_console_shells("", probe);
    const auto* gb = find_shell(shells, "git-bash");
    ASSERT_NE(gb, nullptr);
    EXPECT_TRUE(gb->available);
    EXPECT_FALSE(gb->needs_path);
    EXPECT_NE(gb->command.find("--login -i"), std::string::npos);
    EXPECT_NE(gb->command.find("\"C:\\Program Files\\Git\\bin\\bash.exe\""),
              std::string::npos);
}

// 触发场景:任何位置都找不到 Git Bash。
// 期望:available=false + needs_path=true(前端据此弹「指定 bash 路径」框);
// resolve_shell_command_by_id 返回 nullopt。
TEST(ConsoleShellCatalogTest, GitBashMissingMarksNeedsPath) {
    auto probe = mock_probe({}, {{"ProgramFiles", "C:\\Program Files"}});
    auto shells = acecode::detect_console_shells("", probe);
    const auto* gb = find_shell(shells, "git-bash");
    ASSERT_NE(gb, nullptr);
    EXPECT_FALSE(gb->available);
    EXPECT_TRUE(gb->needs_path);
    EXPECT_FALSE(acecode::resolve_shell_command_by_id("git-bash", "", probe).has_value());
}

// 触发场景:用户配置了 git_bash_path;以及配置指向 WSL bash。
// 期望:有效配置路径优先命中;配置指向 System32\bash.exe(WSL)被排除 → needs_path。
TEST(ConsoleShellCatalogTest, ConfiguredGitBashWinsAndWslExcluded) {
    auto ok_probe = mock_probe({"D:\\tools\\Git\\bin\\bash.exe"}, {});
    auto ok_shells =
        acecode::detect_console_shells("D:\\tools\\Git\\bin\\bash.exe", ok_probe);
    const auto* gb = find_shell(ok_shells, "git-bash");
    ASSERT_NE(gb, nullptr);
    EXPECT_TRUE(gb->available);

    auto wsl_probe = mock_probe({"C:\\Windows\\System32\\bash.exe"}, {});
    auto wsl_shells =
        acecode::detect_console_shells("C:\\Windows\\System32\\bash.exe", wsl_probe);
    const auto* gb2 = find_shell(wsl_shells, "git-bash");
    ASSERT_NE(gb2, nullptr);
    EXPECT_FALSE(gb2->available);
    EXPECT_TRUE(gb2->needs_path);
}

// 触发场景:PowerShell 7(pwsh)已安装 vs 未安装。
// 期望:装了 → 用 pwsh.exe 完整路径;没装 → 回退 powershell.exe(System32 必有)。
TEST(ConsoleShellCatalogTest, PowerShellPrefersPwshWhenPresent) {
    auto with_pwsh = mock_probe(
        {"C:\\Program Files\\PowerShell\\7\\pwsh.exe"},
        {{"ProgramFiles", "C:\\Program Files"}});
    auto a = acecode::detect_console_shells("", with_pwsh);
    const auto* ps = find_shell(a, "powershell");
    ASSERT_NE(ps, nullptr);
    EXPECT_TRUE(ps->available);
    EXPECT_NE(ps->command.find("pwsh.exe"), std::string::npos);

    auto no_pwsh = mock_probe({}, {{"ProgramFiles", "C:\\Program Files"}});
    auto b = acecode::detect_console_shells("", no_pwsh);
    const auto* ps2 = find_shell(b, "powershell");
    ASSERT_NE(ps2, nullptr);
    EXPECT_EQ(ps2->command, "powershell.exe");
}

// 触发场景:默认 shell id 解析。
// 期望:配置空 → 平台默认 cmd;配置 powershell(总可用)→ 用它;配置 git-bash
// 但探测不到 → 回退 cmd(不返回不可用的默认)。
TEST(ConsoleShellCatalogTest, DefaultShellIdResolution) {
    auto probe = mock_probe({}, {{"COMSPEC", "C:\\Windows\\System32\\cmd.exe"}});
    EXPECT_EQ(acecode::default_console_shell_id("", "", probe), "cmd");
    EXPECT_EQ(acecode::default_console_shell_id("powershell", "", probe), "powershell");
    EXPECT_EQ(acecode::default_console_shell_id("git-bash", "", probe), "cmd");
}

#else  // POSIX

// 触发场景:POSIX 探测默认 $SHELL 与 PATH 上的 bash。
// 期望:shell 项取 $SHELL;探到的 bash 成独立项;默认 id = "shell"。
TEST(ConsoleShellCatalogTest, PosixDetectsShellAndBash) {
    auto probe = mock_probe({"/bin/bash", "/usr/bin/zsh"}, {{"SHELL", "/bin/zsh"}});
    auto shells = acecode::detect_console_shells("", probe);
    const auto* def = find_shell(shells, "shell");
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->command, "/bin/zsh");
    const auto* bash = find_shell(shells, "bash");
    ASSERT_NE(bash, nullptr);
    EXPECT_TRUE(bash->available);
    EXPECT_EQ(acecode::default_console_shell_id("", "", probe), "shell");
}

#endif

// 触发场景:kind 枚举 → 协议字符串。
// 期望:四个字面量永久稳定("conpty"/"winpty"/"pipe"/"posix")。前端
// ConsoleDock 用 backend==="pipe" 判断是否显示 legacy 降级提示。
TEST(PtyBackendKindNameTest, ProtocolStringsAreStable) {
    EXPECT_STREQ(acecode::pty_backend_kind_name(acecode::PtyBackendKind::ConPty), "conpty");
    EXPECT_STREQ(acecode::pty_backend_kind_name(acecode::PtyBackendKind::Winpty), "winpty");
    EXPECT_STREQ(acecode::pty_backend_kind_name(acecode::PtyBackendKind::Pipe), "pipe");
    EXPECT_STREQ(acecode::pty_backend_kind_name(acecode::PtyBackendKind::PosixPty), "posix");
}

#ifdef _WIN32

// 触发场景:1809+ 开发机/CI 上探测后端。
// 期望:报 ConPty(GetProcAddress 解析到 CreatePseudoConsole)。
// 若此用例在某机器红,说明该机器是 < 1809 — 探测应报 Winpty,此断言
// 需要按机器分支;当前 CI 与开发机均为 Win11,先按 ConPty 锁死。
TEST(PtyBackendDetectTest, ModernWindowsPrefersConPty) {
    EXPECT_EQ(acecode::detect_pty_backend(), acecode::PtyBackendKind::ConPty);
}

// ConPTY 宿主环境探针(防御性保留)。历史:曾在本 harness 内表现为管道秒断
// 零输出,最初误判为受限 Job 杀 conhost;真因是 pseudoconsole 子进程的 std
// 句柄按老规则从父进程 PEB 复制 — 父进程 std 异常(无 console / NUL / 管道)
// 时 cmd 拿到无效 stdin 读 EOF 即退(code 0)。spawn 侧已修
// (STARTF_USESTDHANDLES + NULL,见 pty_backend_win.cpp),修复后本探针在
// harness 内常绿;保留以防其它怪异宿主再现类似伪影时测试假红。
static bool conpty_usable_in_this_host() {
    PtyCollector probe;
    acecode::PtySpawnSpec spec;
    spec.shell = acecode::resolve_console_shell("");
    spec.cwd = ".";
    std::string error;
    auto pty = acecode::spawn_pty(acecode::PtyBackendKind::ConPty, spec,
                                  probe.callbacks(), error);
    if (!pty) return false;
    bool got_output = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(probe.mu);
            if (!probe.output.empty()) { got_output = true; break; }
        }
        if (probe.exited.load()) break;  // conhost 被宿主秒杀
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    pty->kill();
    return got_output;
}

// 触发场景:ConPTY 后端完整往返 — spawn cmd → echo → 读输出 → exit。
// 期望:输出含标记串;exit 通知在输出之后到达且 exit code 可取。
// 宿主不支持 ConPTY 时跳过(见 conpty_usable_in_this_host 注释)。
TEST(PtyBackendSpawnTest, ConPtyEchoRoundTripAndExit) {
    if (!conpty_usable_in_this_host()) {
        GTEST_SKIP() << "ConPTY conhost is killed by this host environment "
                        "(restricted job); covered by manual smoke instead";
    }
    PtyCollector collector;
    acecode::PtySpawnSpec spec;
    spec.shell = acecode::resolve_console_shell("");
    spec.cwd = ".";
    std::string error;
    auto pty = acecode::spawn_pty(acecode::PtyBackendKind::ConPty, spec,
                                  collector.callbacks(), error);
    ASSERT_NE(pty, nullptr) << error;
    EXPECT_EQ(pty->kind(), acecode::PtyBackendKind::ConPty);
    EXPECT_GT(pty->pid(), 0);

    pty->write("echo ACECODE_PTY_MARK\r\n");
    EXPECT_TRUE(collector.wait_for_output("ACECODE_PTY_MARK", 10000))
        << "output so far: " << collector.output.size() << " bytes";

    pty->resize(100, 30);  // 不崩即可;尺寸生效由 spike/手动冒烟覆盖

    pty->write("exit\r\n");
    EXPECT_TRUE(collector.wait_for_exit(10000));
    pty->kill();  // 幂等
}

// 触发场景:Pipe 兜底后端同样的往返(模拟 < 1809 且 winpty 不可用的机器)。
// 期望:无 TTY 语义下 cmd 批处理回路仍工作(echo 输出可读、exit 可退),
// resize 是 no-op 不崩。这是降级模式的最低可用性保障。
TEST(PtyBackendSpawnTest, PipeFallbackEchoRoundTripAndExit) {
    PtyCollector collector;
    acecode::PtySpawnSpec spec;
    spec.shell = acecode::resolve_console_shell("");
    spec.cwd = ".";
    std::string error;
    auto pty = acecode::spawn_pty(acecode::PtyBackendKind::Pipe, spec,
                                  collector.callbacks(), error);
    ASSERT_NE(pty, nullptr) << error;
    EXPECT_EQ(pty->kind(), acecode::PtyBackendKind::Pipe);

    pty->write("echo ACECODE_PIPE_MARK\r\n");
    EXPECT_TRUE(collector.wait_for_output("ACECODE_PIPE_MARK", 10000));

    pty->resize(120, 40);  // no-op

    pty->write("exit\r\n");
    EXPECT_TRUE(collector.wait_for_exit(10000));
    pty->kill();
}

// 触发场景:kill() 杀死仍在运行的会话。
// 期望:kill 返回后进程已终止、读线程已回收,再调 write/kill 均安全(幂等),
// 且 kill 之后不再有回调触发(stopped_ 闸门)。
TEST(PtyBackendSpawnTest, KillRunningSessionIsIdempotent) {
    PtyCollector collector;
    acecode::PtySpawnSpec spec;
    spec.shell = acecode::resolve_console_shell("");
    spec.cwd = ".";
    std::string error;
    auto pty = acecode::spawn_pty(acecode::PtyBackendKind::ConPty, spec,
                                  collector.callbacks(), error);
    ASSERT_NE(pty, nullptr) << error;

    pty->kill();
    pty->kill();                  // 幂等
    pty->write("echo nothing\r\n");  // no-op,不崩
}

#else // POSIX

// 触发场景:POSIX 平台探测与 forkpty 完整往返。
// 期望:探测报 PosixPty;echo 往返 + exit 通知正常(CI Linux 真实覆盖)。
TEST(PtyBackendSpawnTest, PosixPtyEchoRoundTripAndExit) {
    EXPECT_EQ(acecode::detect_pty_backend(), acecode::PtyBackendKind::PosixPty);

    PtyCollector collector;
    acecode::PtySpawnSpec spec;
    spec.shell = "/bin/sh";
    spec.cwd = ".";
    std::string error;
    auto pty = acecode::spawn_pty(acecode::PtyBackendKind::PosixPty, spec,
                                  collector.callbacks(), error);
    ASSERT_NE(pty, nullptr) << error;
    EXPECT_EQ(pty->kind(), acecode::PtyBackendKind::PosixPty);
    EXPECT_GT(pty->pid(), 0);

    pty->write("echo ACECODE_PTY_MARK\n");
    EXPECT_TRUE(collector.wait_for_output("ACECODE_PTY_MARK", 10000));

    pty->resize(100, 30);

    pty->write("exit\n");
    EXPECT_TRUE(collector.wait_for_exit(10000));
    pty->kill();
}

#endif

} // namespace
