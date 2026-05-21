#include <gtest/gtest.h>

#include "config/config.hpp"
#include "tool/builtin_tool_registry.hpp"
#include "tool/tool_executor.hpp"

using namespace acecode;

TEST(BuiltinToolRegistry, BrowserToolsDisabledByDefaultInSharedSetupPath) {
    AppConfig config;
    config.web_search.enabled = false;
    config.ace_browser_bridge.enabled = false;
    ToolExecutor tools;

    register_session_builtin_tools(tools, config);

    EXPECT_TRUE(tools.has_tool("bash"));
    EXPECT_TRUE(tools.has_tool("file_read"));
    EXPECT_TRUE(tools.has_tool("task_complete"));
    EXPECT_FALSE(tools.has_tool("browser_status"));
}

TEST(BuiltinToolRegistry, BrowserToolsProgressiveModeRegistersCoreInSharedSetupPath) {
    AppConfig config;
    config.web_search.enabled = false;
    config.ace_browser_bridge.enabled = true;
    config.ace_browser_bridge.tool_mode = "progressive";
    ToolExecutor tools;

    register_session_builtin_tools(tools, config);

    EXPECT_TRUE(tools.has_tool("browser_status"));
    EXPECT_TRUE(tools.has_tool("browser_read_page"));
    EXPECT_TRUE(tools.has_tool("browser_enable"));
    EXPECT_FALSE(tools.has_tool("browser_click"));
}

TEST(BuiltinToolRegistry, BrowserToolsFullModeRegistersInteractionToolsInSharedSetupPath) {
    AppConfig config;
    config.web_search.enabled = false;
    config.ace_browser_bridge.enabled = true;
    config.ace_browser_bridge.tool_mode = "full";
    ToolExecutor tools;

    register_session_builtin_tools(tools, config);

    EXPECT_TRUE(tools.has_tool("browser_status"));
    EXPECT_TRUE(tools.has_tool("browser_click"));
    EXPECT_TRUE(tools.has_tool("browser_network"));
}
