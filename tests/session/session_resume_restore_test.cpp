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

// 用户主动 `!cmd` 落盘的形态:`!cmd`(role=user) + tool_result 伪角色配对。
// resume 后必须把 tool_result 翻译为 user_shell_output —— 这样 main.cpp 的
// 渲染端走全量分支,与实时 `!cmd` 行为一致(用户输入的命令一定要看完整输出)。
// 同时 LLM 上下文走 inject_shell_turn 注入规范结构,user_shell_output 不能进
// agent_loop.messages_(它是 UI-only)。
TEST(SessionResumeRestore, ShellModePairTranslatesToUserShellOutput) {
    ResumeRestoreHarness h;

    acecode::ChatMessage shell_user;
    shell_user.role = "user";
    shell_user.content = "!ls -la";  // ! 前缀触发 shell-mode 配对识别

    acecode::ChatMessage shell_result;
    shell_result.role = "tool_result";  // 伪角色,落盘时由 run_shell 写入
    shell_result.content = "total 4\n.\n..\nfoo.txt\nbar.txt";

    acecode::append_resumed_session_messages({shell_user, shell_result},
                                             h.state,
                                             h.loop_,
                                             h.tools_);

    // chat 视图:shell_user 行 + user_shell_output 行(role 翻译过)。
    ASSERT_EQ(h.state.conversation.size(), 2u);
    EXPECT_EQ(h.state.conversation[0].role, "user");
    EXPECT_EQ(h.state.conversation[0].content, "!ls -la");

    EXPECT_EQ(h.state.conversation[1].role, "user_shell_output")
        << "shell-mode 配对的 tool_result 必须翻译为 user_shell_output,"
           "渲染端才能走全量分支不折叠";
    EXPECT_TRUE(h.state.conversation[1].is_tool);
    EXPECT_EQ(h.state.conversation[1].content,
              "total 4\n.\n..\nfoo.txt\nbar.txt");
    // 不能填 summary —— user_shell_output 渲染分支不读这个字段,填了无意义。
    EXPECT_FALSE(h.state.conversation[1].summary.has_value());

    // LLM 上下文:inject_shell_turn 注入了一条 user 规范消息,而不是 push
    // 原始的 shell_user / shell_result 两条。验证 messages_ 末尾是 inject 的。
    ASSERT_EQ(h.loop_.messages().size(), 1u);
    EXPECT_EQ(h.loop_.messages()[0].role, "user");
    EXPECT_NE(h.loop_.messages()[0].content.find("ls -la"), std::string::npos);
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
