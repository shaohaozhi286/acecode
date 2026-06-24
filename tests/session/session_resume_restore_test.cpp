// 覆盖 resume 入口共用的 glue 层:它不仅要调用 replay_session_messages 生成
// TUI 可见的 tool_call / tool_result 行,还要只把 OpenAI 规范 role 回填给
// AgentLoop,避免 UI-only pseudo role 发给 provider。

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_resume_restore.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool/tool_executor.hpp"
#include "tui_state.hpp"
#include "../agent_loop/stub_provider.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace {

namespace fs = std::filesystem;

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

fs::path make_temp_file(const std::string& body) {
    static std::atomic<int> seq{0};
    fs::path p = fs::temp_directory_path() /
                 ("acecode_resume_file_state_" + std::to_string(++seq) + ".txt");
    std::ofstream ofs(p, std::ios::binary);
    ofs << body;
    return p;
}

acecode::ChatMessage assistant_call(const std::string& id,
                                    const std::string& name,
                                    const nlohmann::json& args) {
    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls = one_tool_call(id, name, args.dump());
    return assistant;
}

acecode::ChatMessage tool_result(const std::string& id, const std::string& content) {
    acecode::ChatMessage tool;
    tool.role = "tool";
    tool.tool_call_id = id;
    tool.content = content;
    return tool;
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

TEST(SessionResumeRestore, ReplaysTranscriptOnlyMessagesWithoutProviderHistory) {
    ResumeRestoreHarness h;

    acecode::ChatMessage audit;
    audit.role = "system";
    audit.content = "[Goal] Continuing: visible context";
    audit.metadata = nlohmann::json{{"transcript_only", true}, {"goal_audit", true}};

    acecode::append_resumed_session_messages({audit},
                                             h.state,
                                             h.loop_,
                                             h.tools_);

    ASSERT_EQ(h.state.conversation.size(), 1u);
    EXPECT_EQ(h.state.conversation[0].role, "system");
    EXPECT_EQ(h.state.conversation[0].content, "[Goal] Continuing: visible context");
    EXPECT_TRUE(h.loop_.messages().empty());
}

TEST(SessionResumeRestore, CompactCheckpointKeepsFullTuiTranscriptButRestoresEffectiveHistory) {
    ResumeRestoreHarness h;

    acecode::ChatMessage old_user;
    old_user.role = "user";
    old_user.content = "old prompt";

    acecode::ChatMessage old_assistant;
    old_assistant.role = "assistant";
    old_assistant.content = "old response";

    acecode::ChatMessage marker;
    marker.role = "system";
    marker.content = "Compacted 2 messages, saved ~100 tokens.";
    marker.metadata = nlohmann::json{{"transcript_only", true}};

    acecode::CompactCheckpoint checkpoint;
    checkpoint.trigger = "manual";
    checkpoint.summary = "old prompt summarized";
    checkpoint.replacement_history = [] {
        acecode::ChatMessage summary;
        summary.role = "system";
        summary.content = "[Conversation summary]\nold prompt summarized";
        return std::vector<acecode::ChatMessage>{summary};
    }();

    acecode::ChatMessage after;
    after.role = "user";
    after.content = "new prompt";

    acecode::append_resumed_session_messages(
        {old_user, old_assistant, marker, acecode::encode_compact_checkpoint(checkpoint), after},
        h.state,
        h.loop_,
        h.tools_);

    ASSERT_EQ(h.state.conversation.size(), 4u);
    EXPECT_EQ(h.state.conversation[0].content, "old prompt");
    EXPECT_EQ(h.state.conversation[1].content, "old response");
    EXPECT_EQ(h.state.conversation[2].content, "Compacted 2 messages, saved ~100 tokens.");
    EXPECT_EQ(h.state.conversation[3].content, "new prompt");

    ASSERT_EQ(h.loop_.messages().size(), 2u);
    EXPECT_EQ(h.loop_.messages()[0].content, "[Conversation summary]\nold prompt summarized");
    EXPECT_EQ(h.loop_.messages()[1].content, "new prompt");
}

TEST(SessionResumeRestore, RestoresFullFileReadBaselineFromTranscript) {
    ResumeRestoreHarness h;
    auto p = make_temp_file("alpha\nbeta\n");

    auto assistant = assistant_call("read-1", "file_read",
                                   nlohmann::json{{"file_path", p.string()}});
    auto tool = tool_result(
        "read-1",
        "alpha\nbeta\n\n<acecode-read-metadata encoding=\"utf-8\" line_endings=\"lf\" range=\"1-2\" />\n");

    acecode::append_resumed_session_messages({assistant, tool}, h.state, h.loop_, h.tools_);

    auto check = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(
        p.string(), "alpha\nbeta\n");
    EXPECT_EQ(check.status, acecode::MtimeTracker::ReadBaselineStatus::Ok);
    EXPECT_TRUE(check.content_unchanged_after_mtime_change);

    fs::remove(p);
}

TEST(SessionResumeRestore, DoesNotRestorePartialReadAsFullBaseline) {
    ResumeRestoreHarness h;
    auto p = make_temp_file("alpha\nbeta\n");

    auto assistant = assistant_call("read-partial", "file_read",
                                   nlohmann::json{
                                       {"file_path", p.string()},
                                       {"start_line", 1},
                                       {"end_line", 1}});
    auto tool = tool_result(
        "read-partial",
        "1: alpha\n<acecode-read-metadata encoding=\"utf-8\" line_endings=\"lf\" range=\"1-1\" />\n");

    acecode::append_resumed_session_messages({assistant, tool}, h.state, h.loop_, h.tools_);

    auto check = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(
        p.string(), "alpha\nbeta\n");
    EXPECT_EQ(check.status, acecode::MtimeTracker::ReadBaselineStatus::NotRead);

    fs::remove(p);
}

TEST(SessionResumeRestore, RestoresFileWriteBaselineFromTranscript) {
    ResumeRestoreHarness h;
    auto p = make_temp_file("new\n");

    auto assistant = assistant_call("write-1", "file_write",
                                   nlohmann::json{
                                       {"file_path", p.string()},
                                       {"content", "new\n"}});
    auto tool = tool_result("write-1", "Updated file: " + p.string());

    acecode::append_resumed_session_messages({assistant, tool}, h.state, h.loop_, h.tools_);

    auto check = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(
        p.string(), "new\n");
    EXPECT_EQ(check.status, acecode::MtimeTracker::ReadBaselineStatus::Ok);

    fs::remove(p);
}

TEST(SessionResumeRestore, RestoresFileEditBaselineFromCurrentDisk) {
    ResumeRestoreHarness h;
    auto p = make_temp_file("alpha\ndelta\n");

    auto assistant = assistant_call("edit-1", "file_edit",
                                   nlohmann::json{
                                       {"file_path", p.string()},
                                       {"old_string", "beta"},
                                       {"new_string", "delta"}});
    auto tool = tool_result("edit-1", "Edited " + p.string());

    acecode::append_resumed_session_messages({assistant, tool}, h.state, h.loop_, h.tools_);

    auto check = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(
        p.string(), "alpha\ndelta\n");
    EXPECT_EQ(check.status, acecode::MtimeTracker::ReadBaselineStatus::Ok);

    fs::remove(p);
}

TEST(SessionResumeRestore, IgnoresUnchangedReadStubDuringBaselineRestore) {
    ResumeRestoreHarness h;
    auto p = make_temp_file("alpha\n");

    auto assistant = assistant_call("read-stub", "file_read",
                                   nlohmann::json{{"file_path", p.string()}});
    auto tool = tool_result(
        "read-stub",
        "File unchanged since last read. The content from the earlier file_read tool result in this conversation is still current; refer to that instead of re-reading.");

    acecode::append_resumed_session_messages({assistant, tool}, h.state, h.loop_, h.tools_);

    auto check = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(
        p.string(), "alpha\n");
    EXPECT_EQ(check.status, acecode::MtimeTracker::ReadBaselineStatus::NotRead);

    fs::remove(p);
}
