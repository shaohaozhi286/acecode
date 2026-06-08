#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "agent_loop_doom_guard.hpp"
#include "permissions.hpp"
#include "stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using acecode::AgentCallbacks;
using acecode::AgentLoop;
using acecode::AgentLoopDoomGuard;
using acecode::ChatMessage;
using acecode::PermissionManager;
using acecode::PermissionResult;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode::ToolCall;
using acecode_test::StubLlmProvider;

namespace {

std::string bash_args(const std::string& command) {
    return nlohmann::json{{"command", command}}.dump();
}

ToolCall make_call(std::string id, std::string tool, std::string args) {
    ToolCall call;
    call.id = std::move(id);
    call.function_name = std::move(tool);
    call.function_arguments = std::move(args);
    return call;
}

ToolResult low_signal_result(std::string output = "(no output)") {
    ToolResult result;
    result.output = std::move(output);
    result.success = false;
    return result;
}

ToolResult useful_result(std::string output = "line 1: match") {
    ToolResult result;
    result.output = std::move(output);
    result.success = true;
    return result;
}

ToolImpl create_fake_bash_tool(std::atomic<int>& executions) {
    ToolDef def;
    def.name = "bash";
    def.description = "Fake bash for doom-loop guard tests.";
    def.parameters = {
        {"type", "object"},
        {"properties", {{"command", {{"type", "string"}}}}},
        {"required", {"command"}}
    };

    ToolImpl impl;
    impl.definition = def;
    impl.execute = [&executions](const std::string& args, const acecode::ToolContext&) {
        ++executions;
        auto parsed = nlohmann::json::parse(args, nullptr, false);
        const std::string command =
            parsed.is_object() && parsed.contains("command") && parsed["command"].is_string()
                ? parsed["command"].get<std::string>()
                : std::string{};
        if (command.find("findstr") != std::string::npos) {
            return ToolResult{"(no output)", false};
        }
        if (command.find("Select-String") != std::string::npos) {
            return ToolResult{"Access denied.", false};
        }
        return ToolResult{"unexpected real execution", true};
    };
    impl.is_read_only = false;
    impl.source = ToolSource::Builtin;
    return impl;
}

class DoomGuardHarness {
public:
    DoomGuardHarness() {
        tools_.register_tool(create_fake_bash_tool(bash_executions_));

        AgentCallbacks cb;
        cb.on_message = [this](const std::string& role,
                               const std::string& content,
                               bool is_tool) {
            std::lock_guard<std::mutex> lk(mu_);
            messages_.push_back({role, content, is_tool});
        };
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_tool_confirm = [this](const std::string&, const std::string&) {
            ++permission_prompts_;
            return PermissionResult::Allow;
        };

        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<AgentLoop>(accessor, tools_, cb, ".", permissions_);
    }

    ~DoomGuardHarness() = default;

    void push_tool(std::string id, std::string command) {
        provider_->push_tool_call("bash", bash_args(command), std::move(id));
    }

    void push_text(std::string text) {
        provider_->push_text(std::move(text));
    }

    bool submit_and_wait() {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("search the file");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, std::chrono::seconds(5), [this] {
            return !busy_;
        });
    }

    int bash_executions() const { return bash_executions_.load(); }
    int permission_prompts() const { return permission_prompts_.load(); }
    int turn_count() const { return provider_->turn_count(); }

    std::vector<ChatMessage> messages() const {
        std::lock_guard<std::mutex> lk(mu_);
        return messages_;
    }

private:
    ToolExecutor tools_;
    PermissionManager permissions_;
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    std::unique_ptr<AgentLoop> loop_;
    std::atomic<int> bash_executions_{0};
    std::atomic<int> permission_prompts_{0};
    mutable std::mutex mu_;
    std::vector<ChatMessage> messages_;
    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
};

} // namespace

TEST(AgentLoopDoomGuard, ExactDuplicateLowSignalCallGetsSyntheticResult) {
    AgentLoopDoomGuard guard;
    ToolCall call = make_call("call-1", "grep", R"({"pattern":"needle","path":"src/app.cpp"})");

    EXPECT_FALSE(guard.maybe_guard(call).has_value());
    guard.record_result(call, low_signal_result());

    ToolCall duplicate = make_call("call-2", "grep", R"({"pattern":"needle","path":"src/app.cpp"})");
    auto guarded = guard.maybe_guard(duplicate);
    ASSERT_TRUE(guarded.has_value());
    EXPECT_FALSE(guarded->success);
    EXPECT_NE(guarded->output.find("[Doom-loop guard]"), std::string::npos);
}

TEST(AgentLoopDoomGuard, UsefulRepeatedCallIsNotGuarded) {
    AgentLoopDoomGuard guard;
    ToolCall call = make_call("call-1", "grep", R"({"pattern":"needle","path":"src/app.cpp"})");

    guard.record_result(call, low_signal_result("not found"));
    guard.record_result(call, useful_result());

    ToolCall duplicate = make_call("call-2", "grep", R"({"pattern":"needle","path":"src/app.cpp"})");
    EXPECT_FALSE(guard.maybe_guard(duplicate).has_value());
}

TEST(AgentLoopDoomGuard, SemanticBashRepeatsAgainstSameTargetTriggerCooldown) {
    AgentLoopDoomGuard guard;
    ToolCall first = make_call("call-1", "bash",
        bash_args(R"(findstr /n "Tab" D:\prd\search.md)"));
    ToolCall second = make_call("call-2", "bash",
        bash_args(R"(powershell -Command "Get-Content D:\prd\search.md | Select-String 'Tab'")"));
    ToolCall third = make_call("call-3", "bash",
        bash_args(R"(powershell -Command "Select-String -Path D:\prd\search.md -Pattern Tab")"));
    ToolCall cooled = make_call("call-4", "bash",
        bash_args(R"(type D:\prd\other.md)"));

    guard.record_result(first, low_signal_result("(no output)"));
    guard.record_result(second, low_signal_result("Access denied."));

    auto guarded = guard.maybe_guard(third);
    ASSERT_TRUE(guarded.has_value());
    EXPECT_NE(guarded->output.find("temporarily cooled down"), std::string::npos);
    guard.record_result(third, *guarded);

    auto cooldown_guarded = guard.maybe_guard(cooled);
    ASSERT_TRUE(cooldown_guarded.has_value());
    EXPECT_NE(cooldown_guarded->output.find("bash tool is temporarily cooled down"), std::string::npos);
}

TEST(AgentLoopDoomGuardIntegration, SemanticBashGuardSkipsExecutionAndContinues) {
    DoomGuardHarness h;
    h.push_tool("call-1", R"(findstr /n "Tab" D:\prd\search.md)");
    h.push_tool("call-2", R"(powershell -Command "Get-Content D:\prd\search.md | Select-String 'Tab'")");
    h.push_tool("call-3", R"(powershell -Command "Select-String -Path D:\prd\search.md -Pattern Tab")");
    h.push_tool("call-4", R"(type D:\prd\other.md)");
    h.push_text("Done with available evidence.");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(h.bash_executions(), 2);
    EXPECT_EQ(h.permission_prompts(), 2);
    EXPECT_EQ(h.turn_count(), 5);

    bool saw_guard_result = false;
    bool saw_final_text = false;
    for (const auto& msg : h.messages()) {
        if (msg.role == "tool_result" &&
            msg.content.find("[Doom-loop guard]") != std::string::npos) {
            saw_guard_result = true;
        }
        if (msg.role == "assistant" &&
            msg.content.find("Done with available evidence.") != std::string::npos) {
            saw_final_text = true;
        }
    }
    EXPECT_TRUE(saw_guard_result);
    EXPECT_TRUE(saw_final_text);
}
