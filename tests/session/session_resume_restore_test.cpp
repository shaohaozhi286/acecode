// 覆盖 resume 入口共用的 glue 层:它不仅要调用 replay_session_messages 生成
// TUI 可见的 tool_call / tool_result 行,还要只把 OpenAI 规范 role 回填给
// AgentLoop,避免 UI-only pseudo role 发给 provider。

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_resume_restore.hpp"
#include "tool/tool_executor.hpp"
#include "tui_state.hpp"
#include "../agent_loop/stub_provider.hpp"

#include <memory>

namespace {

nlohmann::json one_tool_call(const std::string& id,
                             const std::string& name,
                             const std::string& args) {
    return nlohmann::json::array({
        {
            {"id", id},
            {"type", "function"},
            {"function", {
                {"name", name},
                {"arguments", args}
            }}
        }
    });
}

class ResumeRestoreHarness {
public:
    ResumeRestoreHarness()
        : loop_([this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; },
                tools_,
                acecode::AgentCallbacks{},
                ".",
                permissions_) {}

    acecode::TuiState state;
    acecode::ToolExecutor tools_;
    acecode::PermissionManager permissions_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::AgentLoop loop_;
};

} // namespace

TEST(SessionResumeRestore, RestoresCanonicalToolCallsForTuiAndAgentContext) {
    ResumeRestoreHarness h;

    acecode::ChatMessage user;
    user.role = "user";
    user.content = "inspect";

    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls = one_tool_call("c1", "bash", R"({"command":"dir"})");

    acecode::ChatMessage tool;
    tool.role = "tool";
    tool.content = "output";
    tool.tool_call_id = "c1";

    acecode::append_resumed_session_messages({user, assistant, tool},
                                             h.state,
                                             h.loop_,
                                             h.tools_);

    ASSERT_EQ(h.state.conversation.size(), 3u);
    EXPECT_EQ(h.state.conversation[0].role, "user");
    EXPECT_EQ(h.state.conversation[1].role, "tool_call");
    EXPECT_FALSE(h.state.conversation[1].content.empty());
    EXPECT_EQ(h.state.conversation[1].display_override, "bash  dir");
    EXPECT_EQ(h.state.conversation[2].role, "tool_result");
    EXPECT_TRUE(h.state.conversation[2].is_tool);
    EXPECT_EQ(h.state.conversation[2].content, "output");

    ASSERT_EQ(h.loop_.messages().size(), 3u);
    EXPECT_EQ(h.loop_.messages()[1].role, "assistant");
    EXPECT_TRUE(h.loop_.messages()[1].tool_calls.is_array());
    EXPECT_EQ(h.loop_.messages()[2].role, "tool");
}

TEST(SessionResumeRestore, DoesNotPushStandalonePseudoToolResultToProviderHistory) {
    ResumeRestoreHarness h;

    acecode::ChatMessage pseudo;
    pseudo.role = "tool_result";
    pseudo.content = "display only";

    acecode::append_resumed_session_messages({pseudo},
                                             h.state,
                                             h.loop_,
                                             h.tools_);

    ASSERT_EQ(h.state.conversation.size(), 1u);
    EXPECT_EQ(h.state.conversation[0].role, "tool_result");
    EXPECT_TRUE(h.state.conversation[0].is_tool);
    EXPECT_TRUE(h.loop_.messages().empty());
}
