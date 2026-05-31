#define ACE_BROWSER_HOST_NO_MAIN
#include "../../ace-browser-host/src/main.cpp"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <thread>

namespace {

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}

void mark_extension_ready(DaemonState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.extension_connected = true;
    state.version_compatible = true;
    state.protocol_version = kProtocolVersion;
    state.extension_version = "0.1-test";
    state.browser = "chromium";
    state.last_hello = std::chrono::steady_clock::now();
    state.capabilities = default_capabilities(true);
}

std::vector<char*> argv_ptrs(std::vector<std::string>& args) {
    std::vector<char*> pointers;
    pointers.reserve(args.size());
    for (auto& arg : args) pointers.push_back(arg.data());
    return pointers;
}

TEST(AceBrowserHostCli, BuildsAssertRequestWithConditionArgsAndTimeoutBudget) {
    json args = {
        {"condition", "request_completed"},
        {"url", "/api/save"},
        {"status_class", "2xx"},
        {"timeout_ms", 45000},
    };

    const json request = command_request("demo", "assert", args);

    EXPECT_EQ(request["session"], "demo");
    EXPECT_EQ(request["action"], "assert");
    EXPECT_EQ(request["args"]["condition"], "request_completed");
    EXPECT_EQ(request["args"]["status_class"], "2xx");
    EXPECT_EQ(request["command_timeout_ms"], 50000);
}

TEST(AceBrowserHostCli, ParsesBatchStepsArrayFromPayload) {
    std::string error;
    auto steps = parse_batch_steps_payload(
        R"([{"action":"assert","args":{"condition":"element_absent","target":"@e3"}}])",
        error);

    ASSERT_TRUE(steps.has_value()) << error;
    ASSERT_TRUE(steps->is_array());
    ASSERT_EQ(steps->size(), 1u);
    EXPECT_EQ((*steps)[0]["action"], "assert");
    EXPECT_EQ((*steps)[0]["args"]["condition"], "element_absent");

    error.clear();
    auto wrapped = parse_batch_steps_payload(
        R"({"steps":[{"action":"read_page","args":{"mode":"summary"}}]})",
        error);
    ASSERT_TRUE(wrapped.has_value()) << error;
    EXPECT_EQ((*wrapped)[0]["action"], "read_page");
}

TEST(AceBrowserHostCli, ListTabsAllFlagControlsAllArg) {
    // 触发场景:list-tabs 带 --all 时应把 all=true 放进请求 args,让插件枚举浏览器所有 tab
    //(含点击弹出的 popup 新窗口);否则只列本 session adopt 的那个 tab。
    // 期望行为:带 --all -> args 含 all==true;不带 --all -> args 不含 all 键(保持旧行为,不放大枚举范围)。
    std::vector<std::string> with_all = {"ace-browser-host", "list-tabs", "--json", "--session", "demo", "--all"};
    auto with_ptrs = argv_ptrs(with_all);
    const json args_with = list_tabs_args(static_cast<int>(with_ptrs.size()), with_ptrs.data());
    ASSERT_TRUE(args_with.contains("all"));
    EXPECT_TRUE(args_with["all"].get<bool>());

    std::vector<std::string> without_all = {"ace-browser-host", "list-tabs", "--json", "--session", "demo"};
    auto without_ptrs = argv_ptrs(without_all);
    const json args_without = list_tabs_args(static_cast<int>(without_ptrs.size()), without_ptrs.data());
    EXPECT_FALSE(args_without.contains("all"));
}

TEST(AceBrowserHostCli, ComputesBatchCommandTimeoutFromStepBudgets) {
    json steps = json::array({
        {
            {"action", "click"},
            {"args", {
                {"selector", "#save"},
                {"expect", {
                    {"condition", "text_equals"},
                    {"target", "#status"},
                    {"text", "saved"},
                    {"timeout_ms", 7000},
                }},
            }},
        },
        {
            {"action", "assert"},
            {"args", {
                {"condition", "request_completed"},
                {"url", "/api/save"},
                {"status_class", "2xx"},
                {"timeout_ms", 20000},
            }},
        },
        {
            {"action", "read_page"},
            {"args", json::object()},
        },
    });

    const json request = command_request("demo", "batch", {{"steps", steps}});

    // 预算 = batch 余量 5000 + click expect 7000 + assert 20000 + read_page 默认 5000。
    EXPECT_EQ(request["command_timeout_ms"], 37000);
}

TEST(AceBrowserHostCli, ParsesBatchPayloadObjectWithVarsAndFinally) {
    std::string error;
    auto payload = parse_batch_payload(
        R"({"vars":{"status":"done"},"steps":[{"action":"read_page","set":"page"}],"finally":[{"action":"read_page","args":{"mode":"summary"}}]})",
        error);

    ASSERT_TRUE(payload.has_value()) << error;
    EXPECT_EQ((*payload)["vars"]["status"], "done");
    ASSERT_TRUE((*payload)["steps"].is_array());
    EXPECT_EQ((*payload)["steps"][0]["set"], "page");
    ASSERT_TRUE((*payload)["finally"].is_array());
    EXPECT_EQ((*payload)["finally"][0]["action"], "read_page");
}

TEST(AceBrowserHostCli, ParsesInlineBatchStdinInputJsonArgument) {
    std::vector<std::string> args = {
        "ace-browser-host",
        "batch",
        "--stdin-input-json",
        R"({"steps":[{"action":"read_page","args":{"mode":"summary"}}]})",
    };
    auto pointers = argv_ptrs(args);

    auto input = find_arg(static_cast<int>(pointers.size()), pointers.data(), "--stdin-input-json");
    ASSERT_TRUE(input.has_value());

    std::string error;
    auto payload = parse_batch_payload(*input, error);
    ASSERT_TRUE(payload.has_value()) << error;
    EXPECT_EQ((*payload)["steps"][0]["action"], "read_page");
}

TEST(AceBrowserHostCli, ComputesBatchCommandTimeoutWithRetryAndFinally) {
    json steps = json::array({
        {
            {"action", "wait"},
            {"retry", {{"attempts", 3}, {"delay_ms", 500}}},
            {"args", {{"condition", "element_visible"}, {"target", "#save"}, {"timeout_ms", 10000}}},
        },
    });
    json finally_steps = json::array({
        {
            {"action", "assert"},
            {"args", {{"condition", "element_absent"}, {"target", "#overlay"}, {"timeout_ms", 5000}}},
        },
    });

    const json request = command_request("demo", "batch", {{"steps", steps}, {"finally", finally_steps}});

    // 预算 = batch 余量 5000 + wait 10000*3 + retry delay 500*2 + finally assert 5000。
    EXPECT_EQ(request["command_timeout_ms"], 41000);
}

TEST(AceBrowserHostCli, ParsesStructuredLocatorTargetOptions) {
    std::vector<std::string> args = {
        "ace-browser-host",
        "click",
        "--locator",
        R"({"role":"button","name":"Save","within":{"role":"row","name":"BUG-123"}})",
        "--near-text",
        "Status",
        "--exact",
    };
    auto pointers = argv_ptrs(args);

    json parsed;
    put_target_options(parsed, static_cast<int>(pointers.size()), pointers.data());

    ASSERT_TRUE(target_options_valid_or_print(parsed));
    EXPECT_EQ(parsed["locator"]["role"], "button");
    EXPECT_EQ(parsed["locator"]["within"]["name"], "BUG-123");
    EXPECT_EQ(parsed["near_text"], "Status");
    EXPECT_TRUE(parsed["exact"].get<bool>());
}

TEST(AceBrowserHostCli, BuildsScreenshotRequestWithLocatorAndAttachmentRef) {
    json request = command_request("demo", "screenshot", {
        {"output", "C:/tmp/bug.png"},
        {"locator", {{"role", "img"}, {"name", "screenshot"}}},
        {"attachment_ref", "@a1"},
    });

    EXPECT_EQ(request["session"], "demo");
    EXPECT_EQ(request["action"], "screenshot");
    EXPECT_EQ(request["args"]["locator"]["role"], "img");
    EXPECT_EQ(request["args"]["attachment_ref"], "@a1");
}

TEST(AceBrowserHostCli, ClampsCommandTimeoutBudget) {
    json steps = json::array();
    for (int i = 0; i < 10; ++i) {
        steps.push_back({{"action", "wait"}, {"args", {{"condition", "network_idle"}, {"timeout_ms", 60000}}}});
    }

    const json request = command_request("demo", "batch", {{"steps", steps}});

    EXPECT_EQ(request["command_timeout_ms"], kMaxCommandTimeoutMs);
    EXPECT_EQ(clamp_command_timeout_ms(1), kCommandTimeoutMs);
}

TEST(AceBrowserHostCli, DirectCdpStatusEnvelopeIsReadyWithoutExtension) {
    json envelope = success({
        {"running", true},
        {"ready", true},
        {"backend", "direct_cdp"},
        {"direct_cdp_ready", true},
        {"extension_connected", false},
        {"extension_stale", false},
        {"version_compatible", true},
    });

    EXPECT_TRUE(status_envelope_ready(envelope));
}

TEST(AceBrowserHostDaemon, StatusPayloadPrefersDirectCdpWithoutExtension) {
    DaemonState state;
    state.direct_cdp->test_mark_ready();

    json status = daemon_status_payload(state, 52007);

    EXPECT_TRUE(status["ready"].get<bool>());
    EXPECT_EQ(status["backend"], "direct_cdp");
    EXPECT_TRUE(status["direct_cdp_ready"].get<bool>());
    EXPECT_FALSE(status["extension_connected"].get<bool>());
    EXPECT_TRUE(status["capabilities"]["cdp"].get<bool>());
    EXPECT_TRUE(status["capabilities"]["raw_cdp"].get<bool>());
}

TEST(AceBrowserHostDaemon, AckedLongActionIsNotRedelivered) {
    DaemonState state;
    mark_extension_ready(state);

    json command_result;
    std::thread command_thread([&]() {
        command_result = handle_command(state, json{
            {"session", "demo"},
            {"action", "batch"},
            {"args", {{"steps", json::array({{{"action", "wait"}, {"args", {{"condition", "network_idle"}, {"timeout_ms", 45000}}}}})}}},
            {"command_timeout_ms", 50000},
        }.dump());
    });

    ASSERT_TRUE(wait_until([&]() {
        std::lock_guard<std::mutex> lock(state.mutex);
        return !state.queued_actions.empty();
    }));

    const json poll = handle_plugin_poll(state, "{}");
    ASSERT_TRUE(poll.value("ok", false)) << poll.dump();
    ASSERT_TRUE(poll["data"]["action"].is_object()) << poll.dump();
    const std::string id = poll["data"]["action"]["id"].get<std::string>();

    const json ack = handle_plugin_ack(state, json{{"id", id}}.dump());
    ASSERT_TRUE(ack.value("ok", false)) << ack.dump();
    ASSERT_TRUE(ack["data"].value("known", false)) << ack.dump();

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        auto it = state.pending_actions.find(id);
        ASSERT_NE(it, state.pending_actions.end());
        it->second->dispatched_at =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(kRedeliveryAfterMs + 1000);
    }
    state.cv.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        EXPECT_TRUE(state.queued_actions.empty());
        EXPECT_EQ(state.pending_actions.size(), 1u);
    }

    const json plugin_result = handle_plugin_result(state, json{
        {"id", id},
        {"result", {{"ok", true}, {"data", {{"success", true}}}}},
    }.dump());
    ASSERT_TRUE(plugin_result.value("ok", false)) << plugin_result.dump();

    command_thread.join();
    EXPECT_TRUE(command_result.value("ok", false)) << command_result.dump();
}

}  // namespace
