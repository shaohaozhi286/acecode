#include <gtest/gtest.h>

#include "tool/mcp_startup_coordination.hpp"
#include "tool/tool_executor.hpp"

#include <atomic>
#include <chrono>
#include <string>
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

} // namespace

TEST(McpStartupCoordination, FormatsStartupAndStatusMessages) {
    EXPECT_EQ(
        acecode::mcp_background_start_message(2),
        "[MCP] Starting 2 server(s) in the background. External tools will appear as each server is ready.");

    acecode::McpServerInfo connected;
    connected.name = "alpha";
    connected.state = acecode::McpServerState::Connected;
    connected.tool_count = 3;
    auto connected_message = acecode::mcp_status_message(connected);
    ASSERT_TRUE(connected_message.has_value());
    EXPECT_EQ(*connected_message, "[MCP] Server 'alpha' connected, registered 3 tool(s).");

    acecode::McpServerInfo failed;
    failed.name = "bad";
    failed.state = acecode::McpServerState::Failed;
    failed.error = "boom";
    auto failed_message = acecode::mcp_status_message(failed);
    ASSERT_TRUE(failed_message.has_value());
    EXPECT_EQ(*failed_message, "[MCP] Server 'bad' failed: boom");

    acecode::McpServerInfo starting;
    starting.name = "slow";
    starting.state = acecode::McpServerState::Starting;
    EXPECT_FALSE(acecode::mcp_status_message(starting).has_value());

    EXPECT_EQ(
        acecode::mcp_first_turn_still_starting_warning(),
        "[MCP] Some servers are still starting; continuing with the tools that are ready.");
}

TEST(McpStartupCoordination, FirstTurnWaitWarnsOnceWhenStartupDoesNotSettle) {
    auto cfg = config_with_stdio_server(
        "slow",
        helper_args({"--delay-ms", "350", "--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::McpManager manager;
    ASSERT_TRUE(manager.connect_all(cfg));
    manager.start_async(tools);
    ASSERT_TRUE(manager.has_starting_servers());

    std::atomic<bool> wait_done{false};
    auto first = acecode::coordinate_mcp_before_first_turn(
        manager,
        wait_done,
        std::chrono::milliseconds(1));
    EXPECT_FALSE(first.already_done);
    EXPECT_TRUE(first.waited);
    EXPECT_FALSE(first.settled);
    EXPECT_TRUE(first.should_warn);

    auto second = acecode::coordinate_mcp_before_first_turn(
        manager,
        wait_done,
        std::chrono::milliseconds(1));
    EXPECT_TRUE(second.already_done);
    EXPECT_FALSE(second.waited);
    EXPECT_FALSE(second.should_warn);

    manager.shutdown();
}

TEST(McpStartupCoordination, FirstTurnWaitNoopsWithoutConfiguredServers) {
    acecode::McpManager manager;
    std::atomic<bool> wait_done{false};

    auto result = acecode::coordinate_mcp_before_first_turn(
        manager,
        wait_done,
        std::chrono::milliseconds(1));

    EXPECT_FALSE(result.already_done);
    EXPECT_FALSE(result.waited);
    EXPECT_TRUE(result.settled);
    EXPECT_FALSE(result.should_warn);
}
