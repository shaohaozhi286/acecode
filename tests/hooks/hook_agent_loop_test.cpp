#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "hooks/hook_config.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "permissions.hpp"
#include "tool/tool_executor.hpp"
#include "../agent_loop/stub_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

acecode::NormalizedHook make_codex_hook(std::string id,
                                        std::string event,
                                        std::string matcher = "*") {
    acecode::NormalizedHook hook;
    hook.id = std::move(id);
    hook.source_id = "test-source";
    hook.event_name = std::move(event);
    hook.matcher = std::move(matcher);
    hook.kind = acecode::HookHandlerKind::Command;
    hook.command.command = "hook-command";
    hook.command.timeout_seconds = 1;
    hook.trust_status = acecode::HookTrustStatus::Trusted;
    return hook;
}

acecode::HookRegistrySnapshot registry_with(std::vector<acecode::NormalizedHook> hooks) {
    acecode::HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = true;
    snapshot.hooks = std::move(hooks);
    return snapshot;
}

acecode::ToolImpl make_probe_tool(const std::string& name,
                                  bool read_only,
                                  std::atomic<int>* calls,
                                  std::string output = "probe ok") {
    acecode::ToolDef def;
    def.name = name;
    def.description = "hook test probe";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object({
            {"value", {{"type", "string"}}},
        })},
    };
    acecode::ToolImpl impl;
    impl.definition = def;
    impl.is_read_only = read_only;
    impl.execute = [calls, output](const std::string&, const acecode::ToolContext&) {
        if (calls) calls->fetch_add(1);
        return acecode::ToolResult{output, true};
    };
    return impl;
}

acecode::ToolImpl make_capturing_tool(const std::string& name,
                                      bool read_only,
                                      std::atomic<int>* calls,
                                      std::string* captured_args) {
    acecode::ToolDef def;
    def.name = name;
    def.description = "hook test capture";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object({
            {"value", {{"type", "string"}}},
        })},
    };
    acecode::ToolImpl impl;
    impl.definition = def;
    impl.is_read_only = read_only;
    impl.execute = [calls, captured_args](const std::string& args, const acecode::ToolContext&) {
        if (calls) calls->fetch_add(1);
        if (captured_args) *captured_args = args;
        return acecode::ToolResult{args, true};
    };
    return impl;
}

struct LoopHarness {
    explicit LoopHarness(std::shared_ptr<acecode_test::StubLlmProvider> p)
        : provider(std::move(p)) {
        callbacks.on_busy_changed = [this](bool value) {
            std::lock_guard<std::mutex> lk(mu);
            busy = value;
            if (!busy) cv.notify_all();
        };
        callbacks.on_tool_confirm = [this](const std::string&, const std::string&) {
            confirm_count.fetch_add(1);
            return acecode::PermissionResult::Deny;
        };
        loop = std::make_unique<acecode::AgentLoop>(
            [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
            tools,
            callbacks,
            ".",
            permissions);
    }

    bool submit_and_wait(const std::string& text = "hello",
                         std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lk(mu);
            busy = true;
        }
        loop->submit(text);
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [this] { return !busy; });
    }

    std::shared_ptr<acecode_test::StubLlmProvider> provider;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks callbacks;
    std::unique_ptr<acecode::AgentLoop> loop;
    std::atomic<int> confirm_count{0};
    std::mutex mu;
    std::condition_variable cv;
    bool busy = false;
};

class CompactCountingProvider : public acecode::LlmProvider {
public:
    int chat_calls = 0;

    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override {
        ++chat_calls;
        acecode::ChatResponse response;
        response.content = "<summary>compact summary</summary>";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }

    std::string name() const override { return "stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "stub-1"; }
    void set_model(const std::string&) override {}
};

acecode::HookProcessResult hook_json(const std::string& json) {
    acecode::HookProcessResult r;
    r.started = true;
    r.exit_code = 0;
    r.stdout_text = json;
    r.output = json;
    return r;
}

} // namespace

TEST(HookAgentLoop, DispatchesAssistantCompletedAfterTextMessageCommit) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("done");

    acecode::HookConfig hook_cfg;
    hook_cfg.enabled = true;
    acecode::HookDefinition hook;
    hook.id = "capture";
    hook.event = acecode::kHookEventAssistantMessageCompleted;
    hook.mode = acecode::HookMode::Sync;
    hook.command.command = "noop";
    hook_cfg.events[acecode::kHookEventAssistantMessageCompleted].push_back(hook);

    std::mutex mu;
    std::condition_variable cv;
    std::vector<nlohmann::json> payloads;
    acecode::HookManager hooks(std::move(hook_cfg),
        [&](const acecode::HookCommandSpec&,
            const std::string& stdin_text,
            int,
            const std::string&) {
            {
                std::lock_guard<std::mutex> lk(mu);
                payloads.push_back(nlohmann::json::parse(stdin_text));
            }
            cv.notify_all();
            acecode::HookProcessResult r;
            r.started = true;
            r.exit_code = 0;
            return r;
        });

    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    bool busy = false;
    acecode::AgentCallbacks callbacks;
    callbacks.on_busy_changed = [&](bool value) {
        {
            std::lock_guard<std::mutex> lk(mu);
            busy = value;
        }
        cv.notify_all();
    };

    acecode::AgentLoop loop(
        [&provider]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        callbacks,
        ".",
        permissions);
    loop.set_hook_manager(&hooks);
    loop.submit("hello");

    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 5s, [&] {
        return !busy && !payloads.empty();
    }));
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads[0]["event"], acecode::kHookEventAssistantMessageCompleted);
    EXPECT_EQ(payloads[0]["hook_id"], "capture");
    EXPECT_EQ(payloads[0]["assistant"]["kind"], "text");
    EXPECT_EQ(payloads[0]["assistant"]["content"], "done");
    EXPECT_EQ(payloads[0]["model"]["provider"], "stub");
    EXPECT_EQ(payloads[0]["model"]["model"], "stub-1");
}

TEST(HookAgentLoop, UserPromptSubmitBlockPreventsPersistenceAndProviderCall) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("should not run");
    LoopHarness h(provider);

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "prompt-block", acecode::kCodexHookEventUserPromptSubmit)}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            acecode::HookProcessResult r;
            r.started = true;
            r.exit_code = 2;
            r.stderr_text = "blocked prompt";
            return r;
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("blocked"));
    EXPECT_EQ(provider->turn_count(), 0);
    EXPECT_TRUE(h.loop->messages().empty());
}

TEST(HookAgentLoop, UserPromptSubmitAdditionalContextReachesNextRequestOnly) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("done");
    LoopHarness h(provider);

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "prompt-context", acecode::kCodexHookEventUserPromptSubmit)}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"hookSpecificOutput":{"additionalContext":"hook context value"}})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("hello"));
    ASSERT_EQ(provider->turn_count(), 1);
    auto request = provider->messages_for_turn(0);
    bool request_has_context = false;
    for (const auto& msg : request) {
        if (msg.content.find("hook context value") != std::string::npos) {
            request_has_context = true;
        }
    }
    EXPECT_TRUE(request_has_context);
    for (const auto& msg : h.loop->messages()) {
        EXPECT_EQ(msg.content.find("hook context value"), std::string::npos);
    }
}

TEST(HookAgentLoop, SessionStartAdditionalContextReachesNextRequestOnly) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("done");
    LoopHarness h(provider);

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "session-context", acecode::kCodexHookEventSessionStart, "startup")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"hookSpecificOutput":{"additionalContext":"session hook context"}})");
        });
    h.loop->set_hook_manager(&hooks);
    h.loop->dispatch_session_start_hook("startup");

    ASSERT_TRUE(h.submit_and_wait("hello"));
    ASSERT_EQ(provider->turn_count(), 1);
    auto request = provider->messages_for_turn(0);
    bool request_has_context = false;
    for (const auto& msg : request) {
        if (msg.content.find("session hook context") != std::string::npos) {
            request_has_context = true;
        }
    }
    EXPECT_TRUE(request_has_context);
    for (const auto& msg : h.loop->messages()) {
        EXPECT_EQ(msg.content.find("session hook context"), std::string::npos);
    }
}

TEST(HookAgentLoop, PreToolUseDenySkipsToolExecution) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("probe", R"({"value":"x"})", "call-1");
    LoopHarness h(provider);
    std::atomic<int> calls{0};
    h.tools.register_tool(make_probe_tool("probe", true, &calls));

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "pre-deny", acecode::kCodexHookEventPreToolUse, "probe")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"decision":"block","reason":"no probe"})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(calls.load(), 0);
    bool saw_denied_result = false;
    for (const auto& msg : h.loop->messages()) {
        if (msg.role == "tool" &&
            msg.content.find("no probe") != std::string::npos) {
            saw_denied_result = true;
        }
    }
    EXPECT_TRUE(saw_denied_result);
}

TEST(HookAgentLoop, PreToolUseBashMatcherBlocksShellToolWithVisibleReason) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("bash", R"({"cmd":"echo should-not-run"})", "call-1");
    LoopHarness h(provider);
    std::atomic<int> calls{0};
    h.tools.register_tool(make_probe_tool("bash", false, &calls, "shell ok"));

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "pre-deny-bash", acecode::kCodexHookEventPreToolUse, "Bash")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string& stdin_text, int, const std::string&) {
            auto payload = nlohmann::json::parse(stdin_text);
            EXPECT_EQ(payload.value("tool_name", ""), "bash");
            return hook_json(R"({"decision":"block","reason":"shell blocked by hook"})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(calls.load(), 0);
    bool saw_block_reason = false;
    for (const auto& msg : h.loop->messages()) {
        if (msg.role == "tool" &&
            msg.content.find("shell blocked by hook") != std::string::npos) {
            saw_block_reason = true;
        }
    }
    EXPECT_TRUE(saw_block_reason);
}

TEST(HookAgentLoop, PreToolUseUpdatedInputReachesToolExecution) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("probe", R"({"value":"old"})", "call-1");
    LoopHarness h(provider);
    std::atomic<int> calls{0};
    std::string captured_args;
    h.tools.register_tool(make_capturing_tool("probe", true, &calls, &captured_args));

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "pre-update", acecode::kCodexHookEventPreToolUse, "probe")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"hookSpecificOutput":{"updatedInput":{"value":"new"}}})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(calls.load(), 1);
    auto parsed = nlohmann::json::parse(captured_args);
    EXPECT_EQ(parsed["value"], "new");
}

TEST(HookAgentLoop, PermissionRequestAllowSkipsNormalPrompt) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("write_probe", "{}", "call-1");
    LoopHarness h(provider);
    std::atomic<int> calls{0};
    h.tools.register_tool(make_probe_tool("write_probe", false, &calls));

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "perm-allow", acecode::kCodexHookEventPermissionRequest, "write_probe")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"hookSpecificOutput":{"permissionDecision":"allow"}})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(h.confirm_count.load(), 0);
}

TEST(HookAgentLoop, PermissionRequestNoDecisionPreservesNormalPrompt) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("write_probe", "{}", "call-1");
    LoopHarness h(provider);
    std::atomic<int> calls{0};
    h.tools.register_tool(make_probe_tool("write_probe", false, &calls));

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "perm-none", acecode::kCodexHookEventPermissionRequest, "write_probe")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json("{}");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(h.confirm_count.load(), 1);
}

TEST(HookAgentLoop, PermissionRequestDenySkipsNormalPromptAndTool) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("write_probe", "{}", "call-1");
    LoopHarness h(provider);
    std::atomic<int> calls{0};
    h.tools.register_tool(make_probe_tool("write_probe", false, &calls));

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "perm-deny", acecode::kCodexHookEventPermissionRequest, "write_probe")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"hookSpecificOutput":{"permissionDecision":"deny","permissionDecisionReason":"deny write"}})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(h.confirm_count.load(), 0);
}

TEST(HookAgentLoop, PostToolUseCanReplaceModelVisibleToolResult) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("probe", "{}", "call-1");
    LoopHarness h(provider);
    std::atomic<int> calls{0};
    h.tools.register_tool(make_probe_tool("probe", true, &calls, "real output"));

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "post-replace", acecode::kCodexHookEventPostToolUse, "probe")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"continue":false,"hookSpecificOutput":{"feedback":"masked output"}})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(calls.load(), 1);
    bool saw_replacement = false;
    for (const auto& msg : h.loop->messages()) {
        if (msg.role == "tool" && msg.content.find("masked output") != std::string::npos) {
            saw_replacement = true;
        }
    }
    EXPECT_TRUE(saw_replacement);
}

TEST(HookAgentLoop, StopHookBlockContinuesOneAdditionalTurn) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("first");
    provider->push_text("second");
    LoopHarness h(provider);

    std::atomic<int> stop_calls{0};
    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "stop", acecode::kCodexHookEventStop)}),
        acecode::HookProcessRunner{},
        [&stop_calls](const std::string&, const std::string&, int, const std::string&) {
            int n = stop_calls.fetch_add(1);
            if (n == 0) {
                return hook_json(R"({"decision":"block","reason":"continue once"})");
            }
            return hook_json("{}");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(provider->turn_count(), 2);
    EXPECT_GE(stop_calls.load(), 2);
}

TEST(HookAgentLoop, StopHookContinueFalsePreventsContinuation) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("first");
    provider->push_text("second");
    LoopHarness h(provider);

    auto block = make_codex_hook("stop-block", acecode::kCodexHookEventStop);
    block.command.command = "block";
    auto stop = make_codex_hook("stop-false", acecode::kCodexHookEventStop);
    stop.command.command = "continue-false";
    acecode::HookManager hooks(
        registry_with({block, stop}),
        acecode::HookProcessRunner{},
        [](const std::string& command, const std::string&, int, const std::string&) {
            if (command == "block") {
                return hook_json(R"({"decision":"block","reason":"continue requested"})");
            }
            return hook_json(R"({"continue":false,"reason":"do not continue"})");
        });
    h.loop->set_hook_manager(&hooks);

    ASSERT_TRUE(h.submit_and_wait("go"));
    EXPECT_EQ(provider->turn_count(), 1);
}

TEST(HookAgentLoop, PreCompactContinueFalseStopsBeforeProviderCompact) {
    auto provider = std::make_shared<CompactCountingProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    std::mutex mu;
    std::condition_variable cv;
    bool busy = false;
    acecode::AgentCallbacks callbacks;
    callbacks.on_busy_changed = [&](bool value) {
        std::lock_guard<std::mutex> lk(mu);
        busy = value;
        if (!busy) cv.notify_all();
    };
    acecode::AgentLoop loop(
        [provider]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        callbacks,
        ".",
        permissions);
    loop.push_message(acecode::ChatMessage{"user", std::string(900, 'a')});
    loop.push_message(acecode::ChatMessage{"assistant", std::string(900, 'b')});
    for (int i = 0; i < 4; ++i) {
        loop.push_message(acecode::ChatMessage{"user", "keep " + std::to_string(i)});
        loop.push_message(acecode::ChatMessage{"assistant", "kept " + std::to_string(i)});
    }

    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "pre-compact", acecode::kCodexHookEventPreCompact, "manual")}),
        acecode::HookProcessRunner{},
        [](const std::string&, const std::string&, int, const std::string&) {
            return hook_json(R"({"continue":false,"reason":"skip compact"})");
        });
    loop.set_hook_manager(&hooks);

    {
        std::lock_guard<std::mutex> lk(mu);
        busy = true;
    }
    loop.submit_compact();
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return !busy; }));
    EXPECT_EQ(provider->chat_calls, 0);
}

TEST(HookAgentLoop, AutoPreCompactContinueFalseStopsBeforeProviderCompact) {
    auto provider = std::make_shared<CompactCountingProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    std::mutex mu;
    std::condition_variable cv;
    bool busy = false;
    acecode::AgentCallbacks callbacks;
    callbacks.on_busy_changed = [&](bool value) {
        std::lock_guard<std::mutex> lk(mu);
        busy = value;
        if (!busy) cv.notify_all();
    };
    acecode::AgentLoop loop(
        [provider]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        callbacks,
        ".",
        permissions);
    loop.set_context_window(100);
    loop.push_message(acecode::ChatMessage{"user", std::string(900, 'a')});
    loop.push_message(acecode::ChatMessage{"assistant", std::string(900, 'b')});

    std::atomic<int> pre_calls{0};
    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "pre-compact-auto", acecode::kCodexHookEventPreCompact, "auto")}),
        acecode::HookProcessRunner{},
        [&pre_calls](const std::string&, const std::string& stdin_text, int, const std::string&) {
            auto payload = nlohmann::json::parse(stdin_text);
            EXPECT_EQ(payload.value("trigger", ""), "auto");
            pre_calls.fetch_add(1);
            return hook_json(R"({"continue":false,"reason":"skip auto compact"})");
        });
    loop.set_hook_manager(&hooks);

    {
        std::lock_guard<std::mutex> lk(mu);
        busy = true;
    }
    loop.submit("next turn");
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return !busy; }));
    EXPECT_EQ(pre_calls.load(), 1);
    EXPECT_EQ(provider->chat_calls, 0);
}

TEST(HookAgentLoop, PostCompactRunsAfterManualCompact) {
    auto provider = std::make_shared<CompactCountingProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    std::mutex mu;
    std::condition_variable cv;
    bool busy = false;
    acecode::AgentCallbacks callbacks;
    callbacks.on_busy_changed = [&](bool value) {
        std::lock_guard<std::mutex> lk(mu);
        busy = value;
        if (!busy) cv.notify_all();
    };
    acecode::AgentLoop loop(
        [provider]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        callbacks,
        ".",
        permissions);
    loop.push_message(acecode::ChatMessage{"user", std::string(900, 'a')});
    loop.push_message(acecode::ChatMessage{"assistant", std::string(900, 'b')});
    for (int i = 0; i < 4; ++i) {
        loop.push_message(acecode::ChatMessage{"user", "keep " + std::to_string(i)});
        loop.push_message(acecode::ChatMessage{"assistant", "kept " + std::to_string(i)});
    }

    std::atomic<int> post_calls{0};
    acecode::HookManager hooks(
        registry_with({make_codex_hook(
            "post-compact", acecode::kCodexHookEventPostCompact, "manual")}),
        acecode::HookProcessRunner{},
        [&post_calls](const std::string&, const std::string&, int, const std::string&) {
            post_calls.fetch_add(1);
            return hook_json(R"({"continue":false,"reason":"after compact"})");
        });
    loop.set_hook_manager(&hooks);

    {
        std::lock_guard<std::mutex> lk(mu);
        busy = true;
    }
    loop.submit_compact();
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return !busy; }));
    EXPECT_EQ(provider->chat_calls, 1);
    EXPECT_EQ(post_calls.load(), 1);
}
