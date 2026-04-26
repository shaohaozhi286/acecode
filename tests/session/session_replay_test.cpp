// 覆盖 resume 时把磁盘 OpenAI 规范 role 展开为 TUI 渲染期望的伪角色行的所有场景。
// 这是 restore-tool-calls-on-resume 的读盘端核心:任何 role 翻译错误都会让
// resume 后的对话视图与原会话对不上(空白行 / 整条消失 / 顺序错位)。
//
// 设计:替换 ToolExecutor 不需要 stub —— 我们只调静态方法 build_tool_call_preview,
// 但函数签名要求一个 const ToolExecutor& 实例,所以本地构造默认实例即可。

#include <gtest/gtest.h>

#include "session/session_replay.hpp"
#include "session/tool_metadata_codec.hpp"
#include "tool/tool_executor.hpp"
#include "tool/diff_utils.hpp"
#include "provider/llm_provider.hpp"
#include "tui_state.hpp"

#include <nlohmann/json.hpp>

using acecode::ChatMessage;
using acecode::DiffHunk;
using acecode::DiffLine;
using acecode::DiffLineKind;
using acecode::ToolExecutor;
using acecode::ToolSummary;
using acecode::TuiState;
using acecode::encode_tool_hunks;
using acecode::encode_tool_summary;
using acecode::replay_session_messages;

namespace {

// 用于构造规范 assistant + tool_calls 的辅助:返回一个符合 OpenAI schema 的
// tool_calls JSON 数组(单个 tool call)。
nlohmann::json one_tool_call(const std::string& id,
                             const std::string& name,
                             const std::string& args_json) {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json tc;
    tc["id"] = id;
    tc["type"] = "function";
    tc["function"]["name"] = name;
    tc["function"]["arguments"] = args_json;
    arr.push_back(std::move(tc));
    return arr;
}

ToolSummary make_summary() {
    ToolSummary s;
    s.verb   = "Edited";
    s.object = "src/foo.cpp";
    s.metrics = {{"+", "1"}, {"-", "0"}};
    s.icon   = "✎";
    return s;
}

std::vector<DiffHunk> make_hunks() {
    DiffHunk h;
    h.old_start = 1; h.old_count = 1;
    h.new_start = 1; h.new_count = 2;
    DiffLine ctx;
    ctx.kind = DiffLineKind::Context;
    ctx.text = "ctx";
    ctx.old_line_no = 1;
    ctx.new_line_no = 1;
    h.lines.push_back(ctx);
    DiffLine add;
    add.kind = DiffLineKind::Added;
    add.text = "added";
    add.new_line_no = 2;
    h.lines.push_back(add);
    return {h};
}

} // namespace

// 用户消息不变换。
TEST(SessionReplay, UserPassthrough) {
    ChatMessage m;
    m.role = "user";
    m.content = "hi";

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "user");
    EXPECT_EQ(out[0].content, "hi");
    EXPECT_FALSE(out[0].is_tool);
}

// system 消息不变换。
TEST(SessionReplay, SystemPassthrough) {
    ChatMessage m;
    m.role = "system";
    m.content = "[Auto-compact] ...";

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "system");
    EXPECT_EQ(out[0].content, "[Auto-compact] ...");
    EXPECT_FALSE(out[0].is_tool);
}

// 纯文本 assistant,content 非空 + tool_calls 空 → 1 行 {assistant, content, false}。
TEST(SessionReplay, AssistantTextOnly) {
    ChatMessage m;
    m.role = "assistant";
    m.content = "hello world";

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "assistant");
    EXPECT_EQ(out[0].content, "hello world");
    EXPECT_FALSE(out[0].is_tool);
}

// content 空 + tool_calls=1 → 1 行 tool_call 行,display_override 非空。
TEST(SessionReplay, AssistantWithSingleToolCall) {
    ChatMessage m;
    m.role = "assistant";
    m.content = "";
    m.tool_calls = one_tool_call("c1", "bash", R"({"command":"ls"})");

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "tool_call");
    EXPECT_EQ(out[0].content, R"([Tool: bash] {"command":"ls"})");
    EXPECT_TRUE(out[0].is_tool);
    EXPECT_EQ(out[0].display_override, "bash  ls");
}

// content="先看一下" + tool_calls=1 → 2 行,**文本在前 tool_call 在后**。
// 验证视觉顺序与运行时 on_delta+on_message 的累积顺序一致。
TEST(SessionReplay, AssistantWithTextAndToolCall) {
    ChatMessage m;
    m.role = "assistant";
    m.content = "先看一下";
    m.tool_calls = one_tool_call("c1", "file_read", R"({"file_path":"a.cpp"})");

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].role, "assistant");
    EXPECT_EQ(out[0].content, "先看一下");
    EXPECT_EQ(out[1].role, "tool_call");
    EXPECT_TRUE(out[1].is_tool);
    EXPECT_EQ(out[1].display_override, "file_read  a.cpp");
}

// tool_calls 长度 3 → 3 行 tool_call,顺序与数组一致,各自有独立 display_override。
TEST(SessionReplay, AssistantWithParallelToolCalls) {
    ChatMessage m;
    m.role = "assistant";
    m.content = "";
    nlohmann::json arr = nlohmann::json::array();
    auto push_tc = [&](const std::string& id, const std::string& name, const std::string& args) {
        nlohmann::json tc;
        tc["id"] = id;
        tc["type"] = "function";
        tc["function"]["name"] = name;
        tc["function"]["arguments"] = args;
        arr.push_back(std::move(tc));
    };
    push_tc("c1", "file_read", R"({"file_path":"a.cpp"})");
    push_tc("c2", "file_read", R"({"file_path":"b.cpp"})");
    push_tc("c3", "bash",      R"({"command":"echo hi"})");
    m.tool_calls = arr;

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].display_override, "file_read  a.cpp");
    EXPECT_EQ(out[1].display_override, "file_read  b.cpp");
    EXPECT_EQ(out[2].display_override, "bash  echo hi");
}

// metadata.tool_summary 存在 → summary 字段被还原。
TEST(SessionReplay, ToolMessageWithSummaryMetadata) {
    ChatMessage m;
    m.role = "tool";
    m.content = "Edited foo";
    m.tool_call_id = "c1";
    m.metadata = nlohmann::json::object();
    m.metadata["tool_summary"] = encode_tool_summary(make_summary());

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "tool_result");
    EXPECT_TRUE(out[0].is_tool);
    ASSERT_TRUE(out[0].summary.has_value());
    EXPECT_EQ(out[0].summary->verb, "Edited");
    EXPECT_EQ(out[0].summary->object, "src/foo.cpp");
    EXPECT_FALSE(out[0].hunks.has_value());
}

// metadata.tool_hunks 存在 → hunks 字段被还原。
TEST(SessionReplay, ToolMessageWithHunksMetadata) {
    ChatMessage m;
    m.role = "tool";
    m.content = "Edited foo";
    m.tool_call_id = "c1";
    m.metadata = nlohmann::json::object();
    m.metadata["tool_hunks"] = encode_tool_hunks(make_hunks());

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].hunks.has_value());
    EXPECT_EQ(out[0].hunks->size(), 1u);
    EXPECT_FALSE(out[0].summary.has_value());
}

// 同时有 summary 和 hunks → 都被还原。
TEST(SessionReplay, ToolMessageWithBothMetadata) {
    ChatMessage m;
    m.role = "tool";
    m.content = "ok";
    m.metadata = nlohmann::json::object();
    m.metadata["tool_summary"] = encode_tool_summary(make_summary());
    m.metadata["tool_hunks"]   = encode_tool_hunks(make_hunks());

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].summary.has_value());
    ASSERT_TRUE(out[0].hunks.has_value());
}

// 老 session 没 metadata → tool_result 行的 summary/hunks 都为空(优雅降级)。
TEST(SessionReplay, ToolMessageWithoutMetadataFallsBackToFold) {
    ChatMessage m;
    m.role = "tool";
    m.content = "raw output";
    // metadata 默认 null

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "tool_result");
    EXPECT_EQ(out[0].content, "raw output");
    EXPECT_TRUE(out[0].is_tool);
    EXPECT_FALSE(out[0].summary.has_value());
    EXPECT_FALSE(out[0].hunks.has_value());
}

// metadata.tool_summary 类型错 → decode 返回 nullopt → 该字段为空,
// **整个 replay 不崩溃**。
TEST(SessionReplay, ToolMessageWithCorruptMetadataFallsBack) {
    ChatMessage m;
    m.role = "tool";
    m.content = "ok";
    m.metadata = nlohmann::json::object();
    m.metadata["tool_summary"] = "not-an-object";    // 类型错
    m.metadata["tool_hunks"]   = nlohmann::json::array({"not-a-hunk"});

    ToolExecutor tools;
    EXPECT_NO_THROW({
        auto out = replay_session_messages({m}, tools);
        ASSERT_EQ(out.size(), 1u);
        EXPECT_FALSE(out[0].summary.has_value());
        EXPECT_FALSE(out[0].hunks.has_value());
        EXPECT_EQ(out[0].content, "ok"); // content 还在
    });
}

// tool_calls[0].arguments 不是合法 JSON → display_override 空,
// 但 tool_call 行仍 push,显示走 legacy `[Tool: X] ARGS_RAW`。
TEST(SessionReplay, AssistantInvalidToolArgsFallsBack) {
    ChatMessage m;
    m.role = "assistant";
    m.tool_calls = one_tool_call("c1", "bash", "this is not json");

    ToolExecutor tools;
    auto out = replay_session_messages({m}, tools);

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "tool_call");
    EXPECT_TRUE(out[0].display_override.empty());
    EXPECT_NE(out[0].content.find("[Tool: bash]"), std::string::npos);
    EXPECT_NE(out[0].content.find("this is not json"), std::string::npos);
}

// 模拟一次 [user, assistant+tool_calls(2), tool, tool, assistant_text]
// → 输出 [user, tool_call, tool_call, tool_result, tool_result, assistant]。
TEST(SessionReplay, FullTurnSequence) {
    std::vector<ChatMessage> in;

    ChatMessage u;
    u.role = "user";
    u.content = "fix it";
    in.push_back(u);

    ChatMessage a_tc;
    a_tc.role = "assistant";
    nlohmann::json arr = nlohmann::json::array();
    auto push_tc = [&](const std::string& id, const std::string& name, const std::string& args) {
        nlohmann::json tc;
        tc["id"] = id;
        tc["type"] = "function";
        tc["function"]["name"] = name;
        tc["function"]["arguments"] = args;
        arr.push_back(std::move(tc));
    };
    push_tc("c1", "file_read", R"({"file_path":"a.cpp"})");
    push_tc("c2", "file_read", R"({"file_path":"b.cpp"})");
    a_tc.tool_calls = arr;
    in.push_back(a_tc);

    ChatMessage t1;
    t1.role = "tool";
    t1.content = "contents of a.cpp";
    t1.tool_call_id = "c1";
    in.push_back(t1);

    ChatMessage t2;
    t2.role = "tool";
    t2.content = "contents of b.cpp";
    t2.tool_call_id = "c2";
    in.push_back(t2);

    ChatMessage a_text;
    a_text.role = "assistant";
    a_text.content = "I read both files. They look fine.";
    in.push_back(a_text);

    ToolExecutor tools;
    auto out = replay_session_messages(in, tools);

    ASSERT_EQ(out.size(), 6u);
    EXPECT_EQ(out[0].role, "user");
    EXPECT_EQ(out[1].role, "tool_call");
    EXPECT_EQ(out[2].role, "tool_call");
    EXPECT_EQ(out[3].role, "tool_result");
    EXPECT_EQ(out[4].role, "tool_result");
    EXPECT_EQ(out[5].role, "assistant");
}

// 未知 role 原样推入(向前兼容);role=="tool_result"(shell-mode 伪角色)
// 落进未知分支时仍保 is_tool=true,这样老 session 的 shell 输出依然能渲染。
TEST(SessionReplay, UnknownRolePassthrough) {
    ChatMessage m1;
    m1.role = "future_role";
    m1.content = "x";

    ChatMessage m2;
    m2.role = "tool_result";
    m2.content = "shell output";

    ToolExecutor tools;
    auto out = replay_session_messages({m1, m2}, tools);

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].role, "future_role");
    EXPECT_EQ(out[0].content, "x");
    EXPECT_FALSE(out[0].is_tool);

    EXPECT_EQ(out[1].role, "tool_result");
    EXPECT_EQ(out[1].content, "shell output");
    EXPECT_TRUE(out[1].is_tool);
}
