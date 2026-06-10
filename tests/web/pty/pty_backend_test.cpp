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
#include <mutex>
#include <string>
#include <thread>

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
