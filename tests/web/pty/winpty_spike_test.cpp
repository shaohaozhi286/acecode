// winpty 功能冒烟测试(openspec/changes/add-console-dock 任务 1.2 spike 转正)。
//
// 验证 cmake/acecode_winpty.cmake 编出的 libwinpty + winpty-agent.exe 在本机
// 真实可用:起 cmd.exe、写入命令、从 conout 读回输出、resize 不报错。
// winpty 是 Win10 < 1809(无 ConPTY)机器上控制台的完整体验兜底,这条链路
// 坏掉的影响是老系统用户的控制台直接不可用 — 所以做成常驻冒烟而非一次性
// spike 脚本。
//
// agent.exe 由 tests/CMakeLists.txt POST_BUILD 拷到单测 exe 同目录,命中
// findAgentProgram() 未设置覆盖路径时的 fallback(模块同目录)。
//
// 仅 Windows 编译运行;agent 在任何 Windows 版本都能工作(它不依赖 ConPTY),
// 因此本测试在 1809+ 的开发机/CI 上同样有效。
#ifdef _WIN32

#include <gtest/gtest.h>

#include <windows.h>
#include <winpty.h>

#include <chrono>
#include <string>
#include <thread>

#include "web/pty/winpty_agent_location.hpp"

namespace {

// RAII 包一层,失败路径不泄漏句柄。
struct SpikeSession {
    winpty_t* wp = nullptr;
    HANDLE conin = INVALID_HANDLE_VALUE;
    HANDLE conout = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;

    ~SpikeSession() {
        if (process) CloseHandle(process);
        if (conin != INVALID_HANDLE_VALUE) CloseHandle(conin);
        if (conout != INVALID_HANDLE_VALUE) CloseHandle(conout);
        if (wp) winpty_free(wp);
    }
};

std::string error_text(winpty_error_ptr_t err) {
    if (!err) return "(no error)";
    LPCWSTR msg = winpty_error_msg(err);
    std::wstring wide(msg ? msg : L"(null)");
    std::string out(wide.begin(), wide.end()); // 测试诊断用,ASCII 足够
    winpty_error_free(err);
    return out;
}

// 从 conout 持续读,直到累计输出包含 needle 或超时。返回累计内容。
std::string read_until(HANDLE conout, const std::string& needle, int timeout_ms) {
    std::string acc;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD avail = 0;
        if (!PeekNamedPipe(conout, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(conout, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        acc.append(buf, got);
        if (acc.find(needle) != std::string::npos) break;
    }
    return acc;
}

} // namespace

// 触发场景:完整的 winpty 会话往返 — open agent、spawn cmd.exe、写入
// echo 命令、读回输出、resize、退出。
// 期望:conout 能读到 echo 的标记字符串(证明 agent 抓屏→VT 翻译链路通),
// winpty_set_size 成功,cmd 退出后进程句柄可等待。
// 回归意义:任何一步断裂(agent 找不到/管道连不上/编译产物损坏)都会在
// 这里红,而不是等到 <1809 用户现场才发现。
TEST(WinptySpikeTest, SpawnCmdEchoRoundTripAndResize) {
    SpikeSession s;
    winpty_error_ptr_t err = nullptr;

    winpty_config_t* cfg = winpty_config_new(0, &err);
    ASSERT_NE(cfg, nullptr) << error_text(err);
    winpty_config_set_initial_size(cfg, 80, 25);

    s.wp = winpty_open(cfg, &err);
    winpty_config_free(cfg);
    ASSERT_NE(s.wp, nullptr) << "winpty_open failed (agent missing?): "
                             << error_text(err);

    s.conin = CreateFileW(winpty_conin_name(s.wp), GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    s.conout = CreateFileW(winpty_conout_name(s.wp), GENERIC_READ, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(s.conin, INVALID_HANDLE_VALUE);
    ASSERT_NE(s.conout, INVALID_HANDLE_VALUE);

    winpty_spawn_config_t* spawn_cfg = winpty_spawn_config_new(
        WINPTY_SPAWN_FLAG_AUTO_SHUTDOWN, L"C:\\Windows\\System32\\cmd.exe",
        nullptr, nullptr, nullptr, &err);
    ASSERT_NE(spawn_cfg, nullptr) << error_text(err);

    BOOL spawned = winpty_spawn(s.wp, spawn_cfg, &s.process, nullptr, nullptr, &err);
    winpty_spawn_config_free(spawn_cfg);
    ASSERT_TRUE(spawned) << error_text(err);
    ASSERT_NE(s.process, nullptr);

    // 写入 echo 命令。标记串拆开拼接,避免命令行回显先于执行输出命中断言。
    const char* cmd = "echo SPIKE_%READY%_OK\r\n";
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(s.conin, cmd, static_cast<DWORD>(strlen(cmd)),
                          &written, nullptr));

    // cmd 会把 %READY%(未定义变量)原样保留 → 输出 SPIKE_%READY%_OK;
    // 但回显行也含同样文本。无论命中回显还是执行输出,都证明了
    // conin→agent→console→抓屏→conout 的双向链路,对 spike 足够。
    std::string out = read_until(s.conout, "SPIKE_", 10000);
    EXPECT_NE(out.find("SPIKE_"), std::string::npos)
        << "no echo output within timeout; raw bytes: " << out.size();

    // resize 链路(agent 收到调整 console 缓冲尺寸)。
    EXPECT_TRUE(winpty_set_size(s.wp, 100, 30, &err)) << error_text(err);

    // 退出并确认进程句柄可等待(AUTO_SHUTDOWN 下 agent 跟随退出)。
    const char* quit = "exit\r\n";
    WriteFile(s.conin, quit, static_cast<DWORD>(strlen(quit)), &written, nullptr);
    EXPECT_EQ(WaitForSingleObject(s.process, 10000), WAIT_OBJECT_0);
}

// 触发场景:通过 set_winpty_agent_path 指定不存在的 agent 路径后 winpty_open。
// 期望:报错而不是崩溃/挂死,错误码为 AGENT_EXE_MISSING 语义(open 返回空)。
// 回归意义:覆盖我们替换的 findAgentProgram() 实现(winpty_agent_location.cpp)
// 的覆盖-失败分支;测试结束后恢复空覆盖,不影响其它用例的 fallback 行为。
TEST(WinptySpikeTest, MissingAgentOverrideFailsCleanly) {
    acecode::set_winpty_agent_path(L"C:\\nonexistent\\winpty-agent.exe");

    winpty_error_ptr_t err = nullptr;
    winpty_config_t* cfg = winpty_config_new(0, &err);
    ASSERT_NE(cfg, nullptr) << error_text(err);

    winpty_t* wp = winpty_open(cfg, &err);
    winpty_config_free(cfg);
    EXPECT_EQ(wp, nullptr);
    if (wp) winpty_free(wp);
    if (err) winpty_error_free(err);

    acecode::set_winpty_agent_path(L"");
}

#endif // _WIN32
