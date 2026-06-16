#include <gtest/gtest.h>

#include "daemon/mcp_runtime.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
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

} // namespace

TEST(DaemonMcpRuntime, StartReturnsBeforeSlowMcpInitializationAndPublishesToolsLater) {
    auto cfg = config_with_stdio_server(
        "daemon",
        helper_args({"--delay-ms", "250", "--tool", "echo"}));
    acecode::ToolExecutor tools;
    acecode::daemon::DaemonMcpRuntime runtime;

    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(runtime.start(cfg, tools));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    EXPECT_TRUE(runtime.manager().has_starting_servers());
    EXPECT_FALSE(tools.has_tool("mcp_daemon_echo"));

    ASSERT_TRUE(runtime.manager().wait_for_startup_settled(std::chrono::seconds(5)));
    EXPECT_TRUE(tools.has_tool("mcp_daemon_echo"));
}
