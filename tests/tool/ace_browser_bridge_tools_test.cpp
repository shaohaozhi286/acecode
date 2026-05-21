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
            {"cli_version", "cli-test"},
        }},
    };
    nlohmann::json command_response = {
        {"ok", true},
        {"data", {{"success", true}}},
    };
    nlohmann::json screenshot_response = {
        {"ok", true},
        {"data", {{"path", "C:/tmp/page.png"}, {"mimeType", "image/png"}}},
    };
    std::vector<BridgeCliCall> calls;
    std::vector<std::string> actions;

    CliRunner runner() {
        return [self = shared_from_this()](const std::vector<std::string>& argv,
                                           const std::string& stdin_text,
                                           std::chrono::milliseconds) {
            self->calls.push_back(BridgeCliCall{argv, stdin_text});
            if (argv.size() >= 2 && argv[1] == "status") {
                return CliProcessResult{0, false, self->status.dump(), ""};
            }
            if (argv.size() >= 2 && argv[1] == "screenshot") {
                return CliProcessResult{0, false, self->screenshot_response.dump(), ""};
            }
            if (argv.size() >= 2 && argv[1] == "command") {
                auto req = nlohmann::json::parse(stdin_text, nullptr, false);
                if (req.is_object() && req.contains("action")) {
                    self->actions.push_back(req["action"].get<std::string>());
                }
                return CliProcessResult{0, false, self->command_response.dump(), ""};
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
    cfg.cli_path = "fake-ace-browser-cli";
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

    EXPECT_FALSE(tools.has_tool("browser_status"));
    EXPECT_TRUE(tools.get_tool_definitions().empty());
}

TEST(AceBrowserBridgeToolsRegistration, ProgressiveStartsWithCoreAndEnablesGroups) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    EXPECT_TRUE(tools.has_tool("browser_status"));
    EXPECT_TRUE(tools.has_tool("browser_read_page"));
    EXPECT_TRUE(tools.has_tool("browser_enable"));
    EXPECT_FALSE(tools.has_tool("browser_click"));
    EXPECT_FALSE(tools.has_tool("browser_network"));

    ToolResult enabled = tools.execute("browser_enable", R"({"groups":["interaction"]})");

    EXPECT_TRUE(enabled.success);
    EXPECT_TRUE(tools.has_tool("browser_click"));
    EXPECT_TRUE(tools.has_tool("browser_fill"));
    EXPECT_TRUE(tools.has_tool("browser_type"));
    EXPECT_FALSE(tools.has_tool("browser_network"));
}

TEST(AceBrowserBridgeToolsRegistration, CompactModeDoesNotDynamicallyRegisterGroups) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("compact"), fake->runner());

    ToolResult enabled = tools.execute("browser_enable", R"({"groups":["interaction"]})");

    EXPECT_TRUE(enabled.success);
    EXPECT_FALSE(tools.has_tool("browser_click"));
    auto out = result_json(enabled);
    EXPECT_EQ(out["data"]["tool_mode"], "compact");
}

TEST(AceBrowserBridgeToolsRegistration, FullModeRegistersAllBrowserTools) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    for (const auto& name : ace_browser_full_tool_names()) {
        EXPECT_TRUE(tools.has_tool(name)) << name;
    }
}

TEST(AceBrowserBridgeToolsStatus, AddsCapabilitiesAndEnabledGroups) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_status", "{}");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    ASSERT_TRUE(out["data"].contains("capabilities"));
    EXPECT_EQ(out["data"]["capabilities"]["operation_overlay"], true);
    EXPECT_EQ(out["data"]["tool_mode"], "progressive");
    EXPECT_EQ(out["data"]["enabled_tool_groups"][0], "core");
}

TEST(AceBrowserBridgeToolsHealthGate, ExtensionDisconnectedBlocksPageActions) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->status["data"]["extension_connected"] = false;
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_open", R"({"url":"https://example.com"})");

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "extension_not_connected");
    EXPECT_TRUE(fake->actions.empty());
}

TEST(AceBrowserBridgeToolsHealthGate, DaemonStoppedBlocksPageActions) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->status["data"]["running"] = false;
    fake->status["data"]["extension_connected"] = false;
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_read_page", "{}");

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "daemon_not_running");
    EXPECT_TRUE(fake->actions.empty());
}

TEST(AceBrowserBridgeToolsReadPage, CompactsSnapshotAndKeepsElementRefs) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->command_response = {
        {"ok", true},
        {"data", {
            {"url", "https://example.com/form"},
            {"title", "Form"},
            {"tree", nlohmann::json::array({
                {{"ref", "@e1"}, {"role", "textbox"}, {"name", "Name"}},
                {{"ref", "@e15"}, {"role", "button"}, {"text", "Submit"}}
            })},
        }},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_read_page", R"({"mode":"summary"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    EXPECT_TRUE(out["data"].contains("snapshot_id"));
    EXPECT_EQ(out["data"]["url"], "https://example.com/form");
    ASSERT_EQ(out["data"]["elements"].size(), 2u);
    EXPECT_EQ(out["data"]["elements"][1]["ref"], "@e15");
    EXPECT_FALSE(out["data"].contains("tree"));
}

TEST(AceBrowserBridgeToolsValidation, NavigateRejectsInvalidOperationBeforeCliCommand) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_navigate", R"({"operation":"jump"})");

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "invalid_arguments");
    EXPECT_TRUE(fake->calls.empty());
}

TEST(AceBrowserBridgeToolsValidation, NetworkDetailRequiresRequestIdBeforeCliCommand) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute("browser_network", R"({"cmd":"detail"})");

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "invalid_arguments");
    EXPECT_TRUE(fake->calls.empty());
}

TEST(AceBrowserBridgeToolsReadPage, ChangedModeRequiresSnapshotIdBeforeCliCommand) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_read_page", R"({"mode":"changed"})");

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "invalid_arguments");
    EXPECT_TRUE(fake->calls.empty());
}

TEST(AceBrowserBridgeToolsReadPage, PassesReadModeAndSinceSnapshotId) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute(
        "browser_read_page",
        R"({"mode":"changed","since_snapshot_id":"snap_old"})");

    ASSERT_TRUE(result.success);
    ASSERT_GE(fake->calls.size(), 2u);
    auto sent = nlohmann::json::parse(fake->calls.back().stdin_text);
    EXPECT_EQ(sent["action"], "snapshot");
    EXPECT_EQ(sent["args"]["mode"], "changed");
    EXPECT_EQ(sent["args"]["since_snapshot_id"], "snap_old");
}

TEST(AceBrowserBridgeToolsWait, PassesWaitConditionToBridge) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->command_response = {
        {"ok", true},
        {"data", {{"success", true}, {"matched", "element_clickable"}, {"elapsed_ms", 25}}},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute(
        "browser_wait",
        R"({"condition":"element_clickable","target":"@e15","timeout_ms":500})");

    ASSERT_TRUE(result.success);
    ASSERT_GE(fake->calls.size(), 2u);
    auto sent = nlohmann::json::parse(fake->calls.back().stdin_text);
    EXPECT_EQ(sent["action"], "wait");
    EXPECT_EQ(sent["args"]["condition"], "element_clickable");
    EXPECT_EQ(sent["args"]["target"], "@e15");
}

TEST(AceBrowserBridgeToolsSession, CloseSessionFormatsBridgeResult) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->command_response = {
        {"ok", true},
        {"data", {{"success", true}, {"closed", 1}, {"detached", 2}}},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_close_session", R"({"session":"demo"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["data"]["closed"], 1);
    EXPECT_EQ(out["data"]["detached"], 2);
}

TEST(AceBrowserBridgeToolsSession, DefaultsSessionNameWhenNoSessionManager) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_open", R"({"url":"https://example.com"})");

    ASSERT_TRUE(result.success);
    ASSERT_GE(fake->calls.size(), 2u);
    auto sent = nlohmann::json::parse(fake->calls.back().stdin_text);
    EXPECT_EQ(sent["session"], "acecode-default");
    EXPECT_EQ(sent["args"]["session"], "acecode-default");
}

TEST(AceBrowserBridgeToolsTabs, ListTabsPreservesOwnershipMetadata) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->command_response = {
        {"ok", true},
        {"data", {{"success", true}, {"tabs", nlohmann::json::array({
            {{"session", "demo"}, {"tabId", 1}, {"ownership", "owned"}, {"groupTitle", "ACE: demo"}},
            {{"session", "demo"}, {"tabId", 2}, {"ownership", "adopted"}, {"groupTitle", "ACE: demo"}}
        })}}},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute("browser_list_tabs", R"({"session":"demo"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    ASSERT_EQ(out["data"]["tabs"].size(), 2u);
    EXPECT_EQ(out["data"]["tabs"][0]["ownership"], "owned");
    EXPECT_EQ(out["data"]["tabs"][1]["ownership"], "adopted");
}

TEST(AceBrowserBridgeToolsTabs, FindTabPassesTabIdAndMarksAdopted) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->command_response = {
        {"ok", true},
        {"data", {{"success", true}, {"tabId", 42}}},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    ToolResult result = tools.execute("browser_find_tab", R"({"session":"demo","tab_id":42})");

    ASSERT_TRUE(result.success);
    ASSERT_GE(fake->calls.size(), 2u);
    auto sent = nlohmann::json::parse(fake->calls.back().stdin_text);
    EXPECT_EQ(sent["action"], "find_tab");
    EXPECT_EQ(sent["args"]["tab_id"], 42);
    auto out = result_json(result);
    EXPECT_EQ(out["data"]["ownership"], "adopted");
}

TEST(AceBrowserBridgeToolsCapture, ScreenshotSuppressesBase64Data) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->screenshot_response = {
        {"ok", true},
        {"data", {
            {"path", "C:/tmp/page.png"},
            {"mimeType", "image/png"},
            {"data", std::string(1024, 'A')},
        }},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute("browser_screenshot", R"({"output_path":"C:/tmp/page.png"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["data"]["path"], "C:/tmp/page.png");
    EXPECT_FALSE(out["data"].contains("data"));
}

TEST(AceBrowserBridgeToolsCapture, SavePdfSuppressesBase64Data) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->command_response = {
        {"ok", true},
        {"data", {
            {"path", "C:/tmp/page.pdf"},
            {"mimeType", "application/pdf"},
            {"sizeBytes", 12},
            {"data", std::string(1024, 'B')},
        }},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute("browser_save_pdf", R"({"file_name":"page.pdf"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["data"]["path"], "C:/tmp/page.pdf");
    EXPECT_FALSE(out["data"].contains("data"));
}

TEST(AceBrowserBridgeToolsNetwork, DetailSummarizesLargeBody) {
    ToolExecutor tools;
    auto fake = make_fake();
    std::string body(9000, 'x');
    fake->command_response = {
        {"ok", true},
        {"data", {{"requestId", "req-1"}, {"body", body}}},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute("browser_network", R"({"cmd":"detail","request_id":"req-1"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    ASSERT_TRUE(out["data"]["body"].is_string());
    EXPECT_LT(out["data"]["body"].get<std::string>().size(), body.size());
    EXPECT_NE(out["data"]["body"].get<std::string>().find("[truncated]"), std::string::npos);
}

TEST(AceBrowserBridgeToolsUpload, MissingFileIsRejectedBeforeCliCommand) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute(
        "browser_upload",
        R"({"target":"input[type=file]","files":["N:/definitely/missing/file.txt"]})");

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "file_not_found");
    EXPECT_TRUE(fake->calls.empty());
}

TEST(AceBrowserBridgeToolsPointer, PassesConfiguredCustomPointerProfile) {
    ToolExecutor tools;
    auto fake = make_fake();
    AceBrowserBridgeConfig cfg = enabled_config("full");
    cfg.pointer_speed = "custom";
    cfg.pointer_custom.move_duration_ms_min = 111;
    cfg.pointer_custom.move_duration_ms_max = 222;
    cfg.pointer_custom.click_hold_ms_min = 33;
    cfg.pointer_custom.click_hold_ms_max = 44;
    cfg.pointer_custom.jitter_px = 4.5;
    register_ace_browser_bridge_tools(tools, cfg, fake->runner());

    ToolResult result = tools.execute("browser_click", R"({"target":"@e1"})");

    ASSERT_TRUE(result.success);
    ASSERT_GE(fake->calls.size(), 2u);
    auto sent = nlohmann::json::parse(fake->calls.back().stdin_text);
    ASSERT_EQ(sent["action"], "click");
    EXPECT_EQ(sent["args"]["speed"], "custom");
    EXPECT_EQ(sent["args"]["pointer_custom"]["move_duration_ms_min"], 111);
    EXPECT_EQ(sent["args"]["pointer_custom"]["click_hold_ms_max"], 44);
    EXPECT_EQ(sent["args"]["pointer_custom"]["jitter_px"], 4.5);
}

TEST(AceBrowserBridgeToolsPointer, PassesSingleActionPointerOverridesAndDebugFlag) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute(
        "browser_click",
        R"({"target":"@e1","speed":"slow","duration_ms":777,"hold_ms":88,"jitter":3.25,"debug_visualization":true})");

    ASSERT_TRUE(result.success);
    ASSERT_GE(fake->calls.size(), 2u);
    auto sent = nlohmann::json::parse(fake->calls.back().stdin_text);
    EXPECT_EQ(sent["action"], "click");
    EXPECT_EQ(sent["args"]["speed"], "slow");
    EXPECT_EQ(sent["args"]["duration_ms"], 777);
    EXPECT_EQ(sent["args"]["hold_ms"], 88);
    EXPECT_EQ(sent["args"]["jitter"], 3.25);
    EXPECT_EQ(sent["args"]["debug_visualization"], true);
}

TEST(AceBrowserBridgeToolsPointer, PreservesFallbackSummaryFromBridge) {
    ToolExecutor tools;
    auto fake = make_fake();
    fake->command_response = {
        {"ok", true},
        {"data", {{"success", true}, {"mode", "dom"}, {"fallback_reason", "cdp_unavailable"}}},
    };
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute("browser_click", R"({"target":"@e1"})");

    ASSERT_TRUE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["data"]["mode"], "dom");
    EXPECT_EQ(out["data"]["fallback_reason"], "cdp_unavailable");
}

TEST(AceBrowserBridgeToolsPointer, RejectsOsModeByDefaultBeforeCliCommand) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute("browser_click", R"({"target":"@e1","mode":"os"})");

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "os_pointer_disabled");
    EXPECT_TRUE(fake->calls.empty());
}

TEST(AceBrowserBridgeToolsType, RejectsLongTypeTextBeforeCliCommand) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());
    std::string long_text(5000, 'x');
    nlohmann::json args = {{"target", "@e1"}, {"text", long_text}};

    ToolResult result = tools.execute("browser_type", args.dump());

    ASSERT_FALSE(result.success);
    auto out = result_json(result);
    EXPECT_EQ(out["error"]["code"], "text_too_long_for_type");
    EXPECT_TRUE(fake->calls.empty());
}

TEST(AceBrowserBridgeToolsType, PassesClearSubmitKeysAndDelay) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    ToolResult result = tools.execute(
        "browser_type",
        R"({"target":"@e1","text":"abc","clear":true,"submit":true,"keys":["Tab"],"delay_ms":[20,80]})");

    ASSERT_TRUE(result.success);
    ASSERT_GE(fake->calls.size(), 2u);
    auto sent = nlohmann::json::parse(fake->calls.back().stdin_text);
    EXPECT_EQ(sent["action"], "type");
    EXPECT_EQ(sent["args"]["selector"], "@e1");
    EXPECT_EQ(sent["args"]["text"], "abc");
    EXPECT_EQ(sent["args"]["clear"], true);
    EXPECT_EQ(sent["args"]["submit"], true);
    EXPECT_EQ(sent["args"]["keys"][0], "Tab");
    EXPECT_EQ(sent["args"]["delay_ms"][0], 20);
    EXPECT_EQ(sent["args"]["delay_ms"][1], 80);
}

TEST(AceBrowserBridgeToolsDescriptions, DescribePointerModesAndFallback) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("full"), fake->runner());

    bool saw_click = false;
    for (const ToolDef& def : tools.get_tool_definitions()) {
        if (def.name == "browser_click") {
            saw_click = true;
            EXPECT_NE(def.description.find("actual mode"), std::string::npos);
            EXPECT_NE(def.description.find("fallback reason"), std::string::npos);
            EXPECT_NE(def.parameters["properties"]["mode"]["description"].get<std::string>().find("CDP"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_click);
}

TEST(AceBrowserBridgeToolPreview, TruncatesLongBrowserArguments) {
    std::string long_text(120, 'x');
    nlohmann::json args = {{"target", "@e1"}, {"value", long_text}};

    auto preview = ToolExecutor::build_tool_call_preview("browser_fill", args.dump());

    EXPECT_EQ(preview.substr(0, 14), "browser_fill  ");
    EXPECT_NE(preview.find("@e1 <- "), std::string::npos);
    EXPECT_NE(preview.find("..."), std::string::npos);
    EXPECT_LE(preview.size(), static_cast<size_t>(14 + 60));
}

TEST(AceBrowserBridgePrompt, GuidesBuiltInBrowserToolsAndRefs) {
    ToolExecutor tools;
    auto fake = make_fake();
    register_ace_browser_bridge_tools(tools, enabled_config("progressive"), fake->runner());

    std::string prompt = build_system_prompt(tools, "C:/repo");

    EXPECT_NE(prompt.find("# Browser Tools"), std::string::npos);
    EXPECT_NE(prompt.find("built-in `browser_*` tools"), std::string::npos);
    EXPECT_NE(prompt.find("browser_read_page"), std::string::npos);
    EXPECT_NE(prompt.find("@e"), std::string::npos);
    EXPECT_NE(prompt.find("browser_enable"), std::string::npos);
    EXPECT_EQ(prompt.find("curl -s -X POST http://127.0.0.1:52007"), std::string::npos);
}
