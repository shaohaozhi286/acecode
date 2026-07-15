#include <gtest/gtest.h>

#include "config/config.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::vector<std::string> helper_args(std::initializer_list<std::string> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

acecode::AppConfig config_with_stdio_server(const std::string& name,
                                            std::vector<std::string> args,
                                            const std::string& command =
                                                ACECODE_MCP_STDIO_TEST_SERVER_PATH) {
    acecode::AppConfig cfg;
    acecode::McpServerConfig server;
    server.transport = acecode::McpTransport::Stdio;
    server.command = command;
    server.args = std::move(args);
    cfg.mcp_servers[name] = std::move(server);
    return cfg;
}

struct ScopedTempDirectory {
    fs::path path;

    ScopedTempDirectory() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("neutral mcp command test " + std::to_string(unique));
        fs::create_directories(path);
    }

    ~ScopedTempDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool has_state(const std::vector<acecode::McpServerInfo>& infos,
               acecode::McpServerState state) {
    return std::any_of(infos.begin(), infos.end(), [&](const auto& info) {
        return info.state == state;
    });
}

class ScopedCerrCapture {
public:
    ScopedCerrCapture()
        : old_(std::cerr.rdbuf(buffer_.rdbuf())) {}

    ~ScopedCerrCapture() {
        std::cerr.rdbuf(old_);
    }

    std::string str() const {
        return buffer_.str();
    }

private:
    std::ostringstream buffer_;
    std::streambuf* old_ = nullptr;
};

} // namespace

TEST(McpManagerAsync, ConnectAllOnlyRecordsConfiguration) {
    auto cfg = config_with_stdio_server("alpha", helper_args({"--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;

    ASSERT_TRUE(manager.connect_all(cfg));

    EXPECT_EQ(manager.configured_server_count(), 1u);
    EXPECT_EQ(manager.connected_server_count(), 0u);
    EXPECT_EQ(manager.discovered_tool_count(), 0u);
    EXPECT_FALSE(manager.has_starting_servers());
    EXPECT_FALSE(tools.has_tool("mcp_alpha_echo"));
}

TEST(McpManagerAsync, StartAsyncPublishesToolsAndStatusUpdates) {
    auto cfg = config_with_stdio_server(
        "alpha",
        helper_args({"--delay-ms", "60", "--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    std::mutex updates_mu;
    std::vector<acecode::McpServerInfo> updates;
    manager.set_status_callback([&](const acecode::McpServerInfo& info) {
        std::lock_guard<std::mutex> lk(updates_mu);
        updates.push_back(info);
    });

    manager.start_async(tools);

    EXPECT_TRUE(manager.has_starting_servers());
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));
    EXPECT_EQ(manager.connected_server_count(), 1u);
    EXPECT_EQ(manager.discovered_tool_count(), 1u);
    EXPECT_TRUE(tools.has_tool("mcp_alpha_echo"));

    auto result = tools.execute("mcp_alpha_echo", R"({"text":"hello"})");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.output, "hello");

    std::lock_guard<std::mutex> lk(updates_mu);
    EXPECT_TRUE(has_state(updates, acecode::McpServerState::Starting));
    EXPECT_TRUE(has_state(updates, acecode::McpServerState::Connected));
}

#ifdef _WIN32
TEST(McpManagerAsync, WindowsStdioPreservesExecutableAndArgumentBoundaries) {
    ScopedTempDirectory temp;
    const fs::path helper_dir = temp.path / "fixture directory";
    const fs::path helper_path = helper_dir / "neutral stdio fixture.exe";
    ASSERT_TRUE(fs::create_directories(helper_dir));

    std::error_code copy_error;
    ASSERT_TRUE(fs::copy_file(
        fs::path(ACECODE_MCP_STDIO_TEST_SERVER_PATH),
        helper_path,
        fs::copy_options::overwrite_existing,
        copy_error)) << copy_error.message();

    const std::vector<std::string> expected = {
        "value with spaces",
        "embedded \"quote\" value",
        "trailing-backslash\\",
        "%COMSPEC%",
        "%CD%",
        "!NAME!",
        "^",
        "&",
    };
    std::vector<std::string> args = {"--tool", "argv", "--report-args"};
    for (const auto& value : expected) {
        args.push_back("--record-arg");
        args.push_back(value);
    }

    auto cfg = config_with_stdio_server("argv", std::move(args), helper_path.string());
    cfg.mcp_servers["argv"].env["NAME"] = "must-not-expand";
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));
    ASSERT_TRUE(tools.has_tool("mcp_argv_argv"));

    const auto result = tools.execute("mcp_argv_argv", R"({})");
    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(nlohmann::json::parse(result.output).get<std::vector<std::string>>(),
              expected);
    manager.shutdown();
}

TEST(McpManagerAsync, WindowsStdioRetainsBatchScriptCompatibility) {
    ScopedTempDirectory temp;
    const fs::path fixture_dir = temp.path / "fixture directory";
    const fs::path helper_path = fixture_dir / "neutral stdio fixture.exe";
    const fs::path wrapper_path = fixture_dir / "neutral stdio wrapper.cmd";
    ASSERT_TRUE(fs::create_directories(fixture_dir));

    std::error_code copy_error;
    ASSERT_TRUE(fs::copy_file(
        fs::path(ACECODE_MCP_STDIO_TEST_SERVER_PATH),
        helper_path,
        fs::copy_options::overwrite_existing,
        copy_error)) << copy_error.message();

    {
        std::ofstream wrapper(wrapper_path, std::ios::binary);
        ASSERT_TRUE(wrapper.is_open());
        wrapper << "@echo off\r\n"
                << "\"%~dp0neutral stdio fixture.exe\" --tool \"%~1\"\r\n";
        ASSERT_TRUE(wrapper.good());
    }

    auto cfg = config_with_stdio_server("shell", {"shell"}, wrapper_path.string());
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));
    EXPECT_TRUE(tools.has_tool("mcp_shell_shell"));
    manager.shutdown();
}
#endif

TEST(McpManagerAsync, MapsReadOnlyHintTrueFalseAndMissing) {
    auto cfg = config_with_stdio_server(
        "hints",
        helper_args({"--annotation-matrix"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));

    EXPECT_TRUE(tools.is_read_only("mcp_hints_read_only"));
    EXPECT_FALSE(tools.is_read_only("mcp_hints_write_capable"));
    EXPECT_FALSE(tools.is_read_only("mcp_hints_unspecified"));
}

TEST(McpManagerAsync, StartAsyncDoesNotMirrorCppMcpInfoLogsToStderr) {
    auto cfg = config_with_stdio_server("alpha", helper_args({"--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    ScopedCerrCapture capture;
    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));
    manager.shutdown();

    const std::string stderr_text = capture.str();
    EXPECT_EQ(stderr_text.find("Server process started successfully"), std::string::npos);
    EXPECT_EQ(stderr_text.find("Creating MCP stdio client"), std::string::npos);
}

TEST(McpManagerAsync, DisableDuringStartupPreventsStaleToolRegistration) {
    auto cfg = config_with_stdio_server(
        "slow",
        helper_args({"--delay-ms", "180", "--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.has_starting_servers());
    ASSERT_TRUE(manager.disable("slow", tools));

    std::this_thread::sleep_for(std::chrono::milliseconds(1300));

    auto servers = manager.list_servers();
    ASSERT_EQ(servers.size(), 1u);
    EXPECT_EQ(servers[0].state, acecode::McpServerState::Disabled);
    EXPECT_FALSE(tools.has_tool("mcp_slow_echo"));
    EXPECT_EQ(manager.discovered_tool_count(), 0u);
}

// 触发场景:config.mcp_servers['alpha'].disabled=true(用户在设置页关掉了它)。
// 期望:connect_all 把它记成 Disabled 态,start_async 跳过它 —— 全 app 不连接、
// 不注册工具;随后运行时 enable() 能把同一 entry 拉起来并注册工具(免重启)。
// 这是「设置页开关影响整个 app」承诺的后端回归点。
TEST(McpManagerAsync, DisabledConfigServerSkipsStartupUntilRuntimeEnable) {
    auto cfg = config_with_stdio_server("alpha", helper_args({"--tool", "echo"}));
    cfg.mcp_servers["alpha"].disabled = true;
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    // 配置态 disabled → entry 建成 Disabled,start_async 不应把它排进连接。
    {
        auto servers = manager.list_servers();
        ASSERT_EQ(servers.size(), 1u);
        EXPECT_EQ(servers[0].state, acecode::McpServerState::Disabled);
    }

    manager.start_async(tools);
    // Disabled 态不参与启动:没有 starting server,也不会注册工具。
    EXPECT_FALSE(manager.has_starting_servers());
    EXPECT_FALSE(tools.has_tool("mcp_alpha_echo"));
    EXPECT_EQ(manager.discovered_tool_count(), 0u);

    // 运行时开关打开:entry 仍在册,enable 应拉起并最终注册工具。
    ASSERT_TRUE(manager.enable("alpha", tools));
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));
    EXPECT_EQ(manager.connected_server_count(), 1u);
    EXPECT_TRUE(tools.has_tool("mcp_alpha_echo"));
}

TEST(McpManagerAsync, DisableConnectedServerUnregistersToolsAndSnapshots) {
    auto cfg = config_with_stdio_server("alpha", helper_args({"--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));
    ASSERT_TRUE(tools.has_tool("mcp_alpha_echo"));
    ASSERT_EQ(manager.get_tool_definitions().size(), 1u);

    ASSERT_TRUE(manager.disable("alpha", tools));

    EXPECT_FALSE(tools.has_tool("mcp_alpha_echo"));
    EXPECT_TRUE(manager.get_tool_definitions().empty());
    EXPECT_EQ(manager.discovered_tool_count(), 0u);
    auto servers = manager.list_servers();
    ASSERT_EQ(servers.size(), 1u);
    EXPECT_EQ(servers[0].state, acecode::McpServerState::Disabled);
}

TEST(McpManagerAsync, ShutdownDuringStartupReturnsQuicklyAndPreventsLateRegistration) {
    auto cfg = config_with_stdio_server(
        "slow",
        helper_args({"--delay-ms", "250", "--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.has_starting_servers());

    const auto start = std::chrono::steady_clock::now();
    manager.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_FALSE(tools.has_tool("mcp_slow_echo"));
    EXPECT_EQ(manager.configured_server_count(), 0u);
}

TEST(McpManagerAsync, ConnectedNoToolServerHasEmptyToolSnapshot) {
    auto cfg = config_with_stdio_server("empty", helper_args({"--no-tools"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));

    EXPECT_EQ(manager.connected_server_count(), 1u);
    EXPECT_EQ(manager.discovered_tool_count(), 0u);
    auto grouped = manager.list_tools_by_server();
    ASSERT_EQ(grouped.size(), 1u);
    EXPECT_EQ(grouped[0].first, "empty");
    EXPECT_TRUE(grouped[0].second.empty());
}

// 触发场景:MCP 工具单次调用耗时很长(实测 windows-mcp 的 Snapshot 30+ 秒),
// 用户点停止/Esc。修复前 invoke 在本线程硬等 cpp-mcp 的 60s 超时,abort 标志
// 只能等工具自己返回才被检查 —— daemon 表现为「点停止提示已终止,busy 却持续
// 到工具返回,切走再切回看到任务还在跑」(2026-07-10 daemon 日志复盘)。
// 期望:abort 置位后 invoke 在百毫秒级返回 [Aborted] 失败结果,不等服务器;
// 迟到的真实响应由 detached 工作线程收下后整体丢弃。
// 阈值说明:服务器 call 延迟 3000ms,300ms 后置 abort;断言总耗时 < 2000ms
// —— 远小于服务器延迟即证明没有等到真实响应,又给轮询(100ms 粒度)和线程
// 调度留足余量,CI 慢机不至于假红。
TEST(McpManagerAsync, AbortDuringSlowToolCallReturnsQuickly) {
    auto cfg = config_with_stdio_server(
        "slow",
        helper_args({"--call-delay-ms", "3000", "--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));
    ASSERT_TRUE(tools.has_tool("mcp_slow_echo"));

    std::atomic<bool> abort_flag{false};
    acecode::ToolContext ctx;
    ctx.abort_flag = &abort_flag;

    std::thread aborter([&abort_flag]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        abort_flag = true;
    });

    const auto start = std::chrono::steady_clock::now();
    auto result = tools.execute("mcp_slow_echo", R"({"text":"hi"})", ctx);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    aborter.join();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.output.rfind("[Aborted]", 0), 0u) << result.output;
    EXPECT_LT(elapsed_ms, 2000) << "abort 应在百毫秒级打断慢工具调用";

    // 给 detached 工作线程留时间收下迟到响应,再拆 manager —— 避免测试进程
    // 退出时工作线程还握着 client 引用(生产环境无此约束,shared_ptr 保活)。
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    manager.shutdown();
}

TEST(McpManagerAsync, FailedStartupRecordsFailureText) {
    acecode::AppConfig cfg;
    acecode::McpServerConfig server;
    server.transport = acecode::McpTransport::Stdio;
    server.command = ACECODE_MCP_STDIO_TEST_SERVER_PATH;
    server.args = helper_args({"--fail-initialize"});
    cfg.mcp_servers["bad"] = std::move(server);

    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));

    manager.start_async(tools);
    ASSERT_TRUE(manager.wait_for_startup_settled(std::chrono::seconds(5)));

    auto servers = manager.list_servers();
    ASSERT_EQ(servers.size(), 1u);
    EXPECT_EQ(servers[0].state, acecode::McpServerState::Failed);
    EXPECT_NE(servers[0].error.find("initialization failed"), std::string::npos);
    EXPECT_FALSE(tools.has_tool("mcp_bad_echo"));
}
