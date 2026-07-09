#include <gtest/gtest.h>

#include "config/config.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/tool_executor.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> helper_args(std::initializer_list<std::string> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

acecode::AppConfig config_with_stdio_server(const std::string& name,
                                            std::vector<std::string> args) {
    acecode::AppConfig cfg;
    acecode::McpServerConfig server;
    server.transport = acecode::McpTransport::Stdio;
    server.command = ACECODE_MCP_STDIO_TEST_SERVER_PATH;
    server.args = std::move(args);
    cfg.mcp_servers[name] = std::move(server);
    return cfg;
}

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
