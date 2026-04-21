// 守护测试:验证 ChatMessage 上的"运行时字段"不会被序列化到 session JSONL。
// 这是未来"新增一个运行时字段但不小心加到 allowlist"的兜底报警 —— 如果
// 有人把 display_override / 类似的字段也写进 JSON,这个测试会红,提醒
// 人去删掉。
//
// 关于 `hunks` 字段的定位:`hunks` 只存在于 `ToolResult` 与 `TuiState::Message`
// 上,**不**在 `ChatMessage` 上。因此 session 序列化通路根本见不到 hunks,
// 天然不可能把它写入 JSONL。这里通过一条注释 + 结构化断言的方式把这个
// 不变式显式化:如果有人未来真的往 ChatMessage 上加 hunks,这个测试不会
// 直接捕获(需要加编译期静态断言),但会提醒维护者回来补一条守护。

#include <gtest/gtest.h>

#include "session/session_serializer.hpp"
#include "provider/llm_provider.hpp"

#include <nlohmann/json.hpp>

using acecode::ChatMessage;
using acecode::serialize_message;

// 场景:`display_override` 是 ChatMessage 的运行时字段(tool_call 行的
// 紧凑预览),TUI 会消费,但不应写入磁盘。构造带值的消息 → 序列化 →
// 解析回 JSON,确认 `display_override` key 不存在。
TEST(SessionSerializerRuntime, DisplayOverrideNotSerialized) {
    ChatMessage msg;
    msg.role = "assistant";
    msg.content = "some reply";
    msg.display_override = "file_edit  src/foo.cpp";

    auto line = serialize_message(msg);
    auto j = nlohmann::json::parse(line);

    EXPECT_FALSE(j.contains("display_override"));
    // 同时确认既有序列化字段仍然正常工作,避免误伤。
    EXPECT_EQ(j.value("role", ""), "assistant");
    EXPECT_EQ(j.value("content", ""), "some reply");
}

// 场景:空 display_override 也不应出现(避免产生 "display_override": "" 噪声)。
TEST(SessionSerializerRuntime, EmptyDisplayOverrideStillAbsent) {
    ChatMessage msg;
    msg.role = "user";
    msg.content = "hi";
    // display_override 保持默认(空串)

    auto line = serialize_message(msg);
    auto j = nlohmann::json::parse(line);

    EXPECT_FALSE(j.contains("display_override"));
}
