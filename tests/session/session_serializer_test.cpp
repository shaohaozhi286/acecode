// 覆盖 src/session/session_serializer.cpp 的 JSONL 编解码:
//   - user / assistant (带 tool_calls) / tool (带 tool_call_id) 三种角色 roundtrip
//   - 空字段必须按约定省略(不产生 "content": "" 之类的多余键)
// 这是会话持久化到磁盘的全部 schema 入口,roundtrip 一旦破坏,之前落盘
// 的 JSONL 文件在新版本下可能无法恢复到一致的 ChatMessage 结构。

#include <gtest/gtest.h>

#include "session/session_serializer.hpp"
#include "provider/llm_provider.hpp"

#include <nlohmann/json.hpp>

using acecode::ChatMessage;
using acecode::serialize_message;
using acecode::deserialize_message;

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
