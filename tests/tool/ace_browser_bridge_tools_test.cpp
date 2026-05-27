#include <gtest/gtest.h>

#include "tool/ace_browser_bridge/browser_tools.hpp"
#include "tool/tool_executor.hpp"
#include "prompt/system_prompt.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace acecode;
using namespace acecode::ace_browser_bridge;

namespace {

struct BridgeCliCall {
    std::vector<std::string> argv;
    std::string stdin_text;
};

struct FakeBridgeCli {
    nlohmann::json status = {
        {"ok", true},
        {"data", {
            {"running", true},
            {"extension_connected", true},
            {"version", "daemon-test"},
            {"extension_version", "extension-test"},
            {"host_version", "host-test"},
        }},
    };
    std::vector<BridgeCliCall> calls;

    CliRunner runner() {
        return [self = shared_from_this()](const std::vector<std::string>& argv,
                                           const std::string& stdin_text,
                                           std::chrono::milliseconds) {
            self->calls.push_back(BridgeCliCall{argv, stdin_text});
            if (argv.size() >= 2 && argv[1] == "status") {
                return CliProcessResult{0, false, self->status.dump(), ""};
            }
            return CliProcessResult{2, false, R"({"ok":false,"error":{"code":"bad_argv","message":"bad argv"}})", ""};
        };
    }

    std::shared_ptr<FakeBridgeCli> shared_from_this() {
        return std::shared_ptr<FakeBridgeCli>(this, [](FakeBridgeCli*) {});
    }
};

AceBrowserBridgeConfig enabled_config(std::string mode = "progressive") {
    AceBrowserBridgeConfig cfg;
    cfg.enabled = true;
    cfg.tool_mode = std::move(mode);
    cfg.host_path = "fake-ace-browser-host";
    return cfg;
}

std::shared_ptr<FakeBridgeCli> make_fake() {
    return std::make_shared<FakeBridgeCli>();
}

nlohmann::json result_json(const ToolResult& result) {
    auto parsed = nlohmann::json::parse(result.output, nullptr, false);
    EXPECT_FALSE(parsed.is_discarded()) << result.output;
    return parsed;
}

} // namespace

TEST(AceBrowserBridgeToolsRegistration, DisabledConfigRegistersNoBrowserTools) {
    ToolExecutor tools;
    AceBrowserBridgeConfig cfg;
    cfg.enabled = false;

    register_ace_browser_bridge_tools(tools, cfg);

    EXPECT_FALSE(tools.has_tool("browser_start"));
    EXPECT_TRUE(tools.get_tool_definitions().empty());
}

TEST(AceBrowserBridgeToolsRegistration, EnabledRegistersOnlyBrowserStart) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    EXPECT_TRUE(tools.has_tool("browser_start"));
    EXPECT_FALSE(tools.has_tool("browser_status"));
    EXPECT_FALSE(tools.has_tool("browser_open"));
    EXPECT_FALSE(tools.has_tool("browser_read_page"));
    EXPECT_FALSE(tools.has_tool("browser_click"));
    EXPECT_EQ(ace_browser_core_tool_names(), (std::vector<std::string>{"browser_start"}));
    EXPECT_TRUE(ace_browser_group_names().empty());
}

TEST(AceBrowserBridgeToolsStart, ReturnsStatusAndCliPrompt) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_start", R"({"session":"demo"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["data"]["session"], "demo");
    EXPECT_EQ(out["data"]["tool"], "browser_start");
    EXPECT_EQ(out["data"]["cli"]["default_port"], 52007);
    ASSERT_TRUE(result.post_user_prompt.has_value());
    EXPECT_NE(result.post_user_prompt->find("ace-browser-host"), std::string::npos);
    EXPECT_NE(result.post_user_prompt->find("block-input"), std::string::npos);
    EXPECT_NE(result.post_user_prompt->find("unblock-input"), std::string::npos);
    EXPECT_NE(result.post_user_prompt->find("read-page"), std::string::npos);
    EXPECT_NE(result.post_user_prompt->find("If the active model cannot inspect images"),
              std::string::npos);
}

TEST(AceBrowserBridgeToolsStart, EmitsPromptOnlyOncePerSessionUnlessForced) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult first = tools.execute("browser_start", R"({"session":"demo"})");
    ToolResult second = tools.execute("browser_start", R"({"session":"demo"})");
    ToolResult forced = tools.execute("browser_start", R"({"session":"demo","force_prompt":true})");

    EXPECT_TRUE(first.post_user_prompt.has_value());
    EXPECT_FALSE(second.post_user_prompt.has_value());
    EXPECT_TRUE(forced.post_user_prompt.has_value());
}

TEST(AceBrowserBridgeToolsStart, CanSuppressPrompt) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_start", R"({"session":"demo","include_prompt":false})");

    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.post_user_prompt.has_value());
}

TEST(AceBrowserBridgeToolsStart, PropagatesHostFailureAndStillLoadsPrompt) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->status = {
        {"ok", false},
        {"error", {{"code", "host_not_found"}, {"message", "missing host"}}},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_start", R"({"session":"demo"})");

    EXPECT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "host_not_found");
    EXPECT_TRUE(result.post_user_prompt.has_value());
}

TEST(AceBrowserBridgeToolsRegistration, UnregisterRemovesStartAndLegacyNames) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());
    ASSERT_TRUE(tools.has_tool("browser_start"));

    const std::size_t removed = unregister_ace_browser_bridge_tools(tools);

    EXPECT_GE(removed, 1u);
    EXPECT_FALSE(tools.has_tool("browser_start"));
    EXPECT_FALSE(tools.has_tool("browser_status"));
}

TEST(AceBrowserBridgePrompt, StaticSystemPromptDoesNotIncludeBrowserGuidance) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    std::string prompt = build_system_prompt(tools, ".");

    EXPECT_EQ(prompt.find("# Browser Tools"), std::string::npos);
    EXPECT_EQ(prompt.find("browser_read_page"), std::string::npos);
    EXPECT_EQ(prompt.find("browser_enable"), std::string::npos);
}
