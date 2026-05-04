// 覆盖 src/session/session_serializer.cpp 的 JSONL 编解码:
//   - user / assistant (带 tool_calls) / tool (带 tool_call_id) 三种角色 roundtrip
//   - 空字段必须按约定省略(不产生 "content": "" 之类的多余键)
// 这是会话持久化到磁盘的全部 schema 入口,roundtrip 一旦破坏,之前落盘
// 的 JSONL 文件在新版本下可能无法恢复到一致的 ChatMessage 结构。

#include <gtest/gtest.h>

#include "session/session_serializer.hpp"
#include "session/session_storage.hpp"
#include "provider/llm_provider.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

using acecode::ChatMessage;
using acecode::serialize_message;
using acecode::deserialize_message;

// 场景:agentic-loop-terminator 的 nudge 消息 —— 以 [acecode:auto-continue]
// 前缀的普通 user 消息,is_meta 必须保持 false 才能通过 normalize_messages_for_api
// 进入 LLM 上下文。这里验证 serialize/deserialize 不篡改这两个字段。
TEST(SessionSerializer, AutoContinueNudgeRoundtrip) {
    ChatMessage in;
    in.role = "user";
    in.content = "[acecode:auto-continue] Continue with the task. If the task is "
                 "fully complete, call task_complete. If you need the user's input, "
                 "call AskUserQuestion. Otherwise keep working.";
    in.is_meta = false;  // 关键:必须 false,否则 LLM 在下一轮看不到 nudge

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    EXPECT_EQ(out.role, "user");
    EXPECT_EQ(out.content, in.content);
    EXPECT_FALSE(out.is_meta);  // is_meta=false 的字段 serializer 会省略,
                                 // 反序列化默认也是 false,roundtrip 必须保留此属性
    EXPECT_TRUE(out.tool_call_id.empty());
    // serialize_message 对 is_meta=false 不落 JSON,这是现有 schema 的既定行为 ——
    // 断言序列化后的字符串里不应该出现 is_meta 字段名,以免将来误改。
    EXPECT_EQ(line.find("is_meta"), std::string::npos);
}

// 场景:最简单的用户消息(role=user, content=...),roundtrip 后字段完全
// 一致且没有多余的 tool_calls / tool_call_id 残留。
TEST(SessionSerializer, UserMessageRoundtrip) {
    ChatMessage in;
    in.role = "user";
    in.content = "fix the bug";

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    EXPECT_EQ(out.role, "user");
    EXPECT_EQ(out.content, "fix the bug");
    EXPECT_TRUE(out.tool_call_id.empty());
    EXPECT_TRUE(out.tool_calls.is_null() || out.tool_calls.empty());
}

// 场景:assistant 纯工具调用(content 为空 + 非空 tool_calls 数组)。
// tool_calls 的嵌套结构(id / function.name / function.arguments)必须
// 原样保留——AgentLoop 要用这些字段重放工具调用。
TEST(SessionSerializer, AssistantWithToolCalls) {
    ChatMessage in;
    in.role = "assistant";
    in.content = "";  // no text reply, pure tool call
    in.tool_calls = nlohmann::json::array({
        {
            {"id", "call_abc123"},
            {"type", "function"},
            {"function", {
                {"name", "bash_tool"},
                {"arguments", "{\"command\":\"ls\"}"}
            }}
        }
    });

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    EXPECT_EQ(out.role, "assistant");
    EXPECT_TRUE(out.content.empty());
    ASSERT_TRUE(out.tool_calls.is_array());
    ASSERT_EQ(out.tool_calls.size(), 1u);
    EXPECT_EQ(out.tool_calls[0]["id"], "call_abc123");
    EXPECT_EQ(out.tool_calls[0]["function"]["name"], "bash_tool");
    EXPECT_EQ(out.tool_calls[0]["function"]["arguments"], "{\"command\":\"ls\"}");
}

// 场景:工具结果消息(role=tool + tool_call_id)roundtrip,tool_call_id
// 必须被保留,否则后续 LLM 请求无法把结果关联回对应的 tool_call。
TEST(SessionSerializer, ToolResultRoundtrip) {
    ChatMessage in;
    in.role = "tool";
    in.content = "total 0\n";
    in.tool_call_id = "call_abc123";

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    EXPECT_EQ(out.role, "tool");
    EXPECT_EQ(out.content, "total 0\n");
    EXPECT_EQ(out.tool_call_id, "call_abc123");
}

// 场景:DeepSeek thinking 模式下 assistant 消息会带 reasoning_content,
// 必须 roundtrip —— 否则 --resume 之后第一次 API 调用会因为缺这个字段被
// DeepSeek 拒绝(400 The reasoning_content in the thinking mode must be
// passed back to the API)。见 openspec/changes/support-deepseek-reasoning。
TEST(SessionSerializer, ReasoningContentRoundTrip) {
    ChatMessage in;
    in.role = "assistant";
    in.content = "the answer";
    in.reasoning_content = "step 1\nstep 2\nstep 3";

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    EXPECT_EQ(out.role, "assistant");
    EXPECT_EQ(out.content, "the answer");
    EXPECT_EQ(out.reasoning_content, "step 1\nstep 2\nstep 3");
}

// 场景:reasoning_content 为空时,JSON 里不应该出现该字段 —— 与 content /
// tool_calls / tool_call_id 一样遵守"空字段省略"约定,避免 JSONL 体积
// 在非 reasoning 模型(Copilot / GPT-4o)的会话里无谓膨胀。
TEST(SessionSerializer, EmptyReasoningContentIsOmitted) {
    ChatMessage in;
    in.role = "assistant";
    in.content = "no thinking here";
    // reasoning_content 默认就是空字符串

    auto line = serialize_message(in);

    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["role"], "assistant");
    EXPECT_FALSE(j.contains("reasoning_content"))
        << "empty reasoning_content must be omitted; got: " << line;

    // 反向也验证一遍:解析回来 reasoning_content 仍是空字符串。
    ChatMessage out = deserialize_message(line);
    EXPECT_EQ(out.reasoning_content, "");
}

// 场景:本次改动之前写入的 legacy JSONL 行没有 reasoning_content 键。
// 反序列化必须不抛异常并把字段保持为空,以保证 --resume 旧会话依然能加载。
TEST(SessionSerializer, LegacyLineWithoutReasoningDeserializesCleanly) {
    // 这是手工构造的 pre-change JSONL 行 —— 故意不含 reasoning_content。
    std::string legacy_line =
        R"({"role":"assistant","content":"old session reply"})";
    ChatMessage out = deserialize_message(legacy_line);

    EXPECT_EQ(out.role, "assistant");
    EXPECT_EQ(out.content, "old session reply");
    EXPECT_EQ(out.reasoning_content, "");
}

// 场景:content 和 tool_calls 均未设置时,序列化出来的 JSON 里必须不含
// 这两个键——否则 JSONL 文件体积会无谓膨胀,也会违背"空字段省略"的
// 既有风格约定。
TEST(SessionSerializer, EmptyContentIsOmitted) {
    ChatMessage in;
    in.role = "assistant";
    // Leave content empty and no tool_calls set — serializer must omit both.
    auto line = serialize_message(in);

    // Parse the emitted line ourselves to inspect keys.
    auto j = nlohmann::json::parse(line);
    EXPECT_EQ(j["role"], "assistant");
    EXPECT_FALSE(j.contains("content"))
        << "empty content must be omitted; got: " << line;
    EXPECT_FALSE(j.contains("tool_calls"))
        << "empty tool_calls must be omitted; got: " << line;
}

// 场景:SessionMeta 含 forked_from / fork_message_id 时,write_meta + read_meta
// roundtrip 后两字段值不变。这是 web fork 持久化的契约。
TEST(SessionSerializer, SessionMetaForkFieldsRoundtrip) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "acecode_meta_fork_test.meta.json";
    fs::remove(tmp);

    acecode::SessionMeta in;
    in.id = "20260503-100000-aaaa";
    in.cwd = "/proj/foo";
    in.created_at = "2026-05-03T10:00:00Z";
    in.updated_at = "2026-05-03T10:01:00Z";
    in.message_count = 5;
    in.summary = "测试 fork";
    in.provider = "copilot";
    in.model = "gpt-4o";
    in.title = "分叉1:重构 auth";
    in.forked_from = "20260503-095500-bbbb";
    in.fork_message_id = "u-abc-123";

    acecode::SessionStorage::write_meta(tmp.string(), in);
    auto out = acecode::SessionStorage::read_meta(tmp.string());
    EXPECT_EQ(out.id, in.id);
    EXPECT_EQ(out.title, in.title);
    EXPECT_EQ(out.forked_from, in.forked_from);
    EXPECT_EQ(out.fork_message_id, in.fork_message_id);

    fs::remove(tmp);
}

// 场景:老 meta 文件不含 forked_from / fork_message_id 字段 → read_meta
// 出来这两字段为空(向后兼容)。
TEST(SessionSerializer, SessionMetaLegacyFileMissingForkFields) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "acecode_meta_legacy_test.meta.json";
    {
        std::ofstream ofs(tmp);
        ofs << R"({
            "id": "20260101-100000-cccc",
            "cwd": "/proj/old",
            "created_at": "2026-01-01T10:00:00Z",
            "updated_at": "2026-01-01T10:00:00Z",
            "message_count": 1,
            "summary": "old session",
            "provider": "openai",
            "model": "gpt-4",
            "title": "old"
        })";
    }
    auto out = acecode::SessionStorage::read_meta(tmp.string());
    EXPECT_EQ(out.id, "20260101-100000-cccc");
    EXPECT_EQ(out.title, "old");
    EXPECT_TRUE(out.forked_from.empty());
    EXPECT_TRUE(out.fork_message_id.empty());

    fs::remove(tmp);
}

// 场景:写一个 forked_from / fork_message_id 都为空的 meta(普通新 session
// 没分叉)→ JSON 里**不应该**出现这两个键(不无谓膨胀)。
TEST(SessionSerializer, SessionMetaEmptyForkFieldsOmitted) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "acecode_meta_empty_fork_test.meta.json";
    fs::remove(tmp);

    acecode::SessionMeta in;
    in.id = "20260503-100000-eeee";
    in.cwd = "/proj";
    in.created_at = "2026-05-03T10:00:00Z";
    in.updated_at = in.created_at;
    in.message_count = 0;
    in.provider = "openai";
    in.model = "gpt-4";
    // forked_from / fork_message_id 故意留空

    acecode::SessionStorage::write_meta(tmp.string(), in);

    nlohmann::json j;
    {
        std::ifstream ifs(tmp);
        std::stringstream ss;
        ss << ifs.rdbuf();
        j = nlohmann::json::parse(ss.str());
    }
    EXPECT_FALSE(j.contains("forked_from"));
    EXPECT_FALSE(j.contains("fork_message_id"));

    std::error_code ec;
    fs::remove(tmp, ec);
}
