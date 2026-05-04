// 覆盖 src/web/message_payload.cpp 的纯函数:
//   - compute_message_id: user 走 uuid;非 user 走 sha1(role+" "+content+" "+timestamp)
//   - chat_message_to_payload_json: 顶层带 id 字段,其它字段 = serialize_message
//
// 这层是 web fork 操作的"消息身份"基础 — 一旦回归,前端拿到的 id 跟 daemon
// 重读 messages 算的 id 不一致,fork 就找不到 message,400 message_not_found。

#include <gtest/gtest.h>

#include "web/message_payload.hpp"
#include "utils/sha1.hpp"
#include "provider/llm_provider.hpp"

#include <nlohmann/json.hpp>

#include <string>

using acecode::ChatMessage;
using acecode::sha1_hex;
using acecode::web::compute_message_id;
using acecode::web::chat_message_to_payload_json;

// 场景:user 消息有 uuid 时,id 就是 uuid;不再走 sha1。
TEST(MessagePayload, UserMessageIdIsUuidWhenPresent) {
    ChatMessage m;
    m.role = "user";
    m.content = "fix the bug";
    m.uuid = "u-12345";
    EXPECT_EQ(compute_message_id(m), "u-12345");
}

// 场景:user 消息没 uuid(老 session 兼容路径),走 sha1 fallback。
TEST(MessagePayload, UserMessageWithoutUuidFallsBackToSha1) {
    ChatMessage m;
    m.role = "user";
    m.content = "hello";
    m.timestamp = "2026-05-03T10:00:00Z";

    auto expected = sha1_hex("user hello 2026-05-03T10:00:00Z");
    EXPECT_EQ(compute_message_id(m), expected);
}

// 场景:assistant 消息走 sha1(role+" "+content+" "+timestamp),小写 hex。
TEST(MessagePayload, AssistantMessageSha1) {
    ChatMessage m;
    m.role = "assistant";
    m.content = "hello";
    m.timestamp = "2026-05-03T10:00:00Z";
    auto expected = sha1_hex("assistant hello 2026-05-03T10:00:00Z");
    EXPECT_EQ(compute_message_id(m), expected);
    // 输出必须全小写 hex
    for (char c : compute_message_id(m)) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    EXPECT_EQ(compute_message_id(m).size(), 40u);
}

// 场景:幂等性 — 同输入两次调用得同输出(否则 GET /messages 两次返回不同 id,
// 前端 fork 用第一次的 id,daemon 第二次找不到 → 400)。
TEST(MessagePayload, IdIsStableAcrossCalls) {
    ChatMessage m;
    m.role = "system";
    m.content = "[Interrupted]";
    auto a = compute_message_id(m);
    auto b = compute_message_id(m);
    EXPECT_EQ(a, b);
}

// 场景:空 content / 空 timestamp 不崩,且不返回空字符串(空 sha1 也是稳定值)。
TEST(MessagePayload, EmptyFieldsDoNotCrash) {
    ChatMessage m;
    m.role = "tool";
    // content 和 timestamp 都空
    auto id = compute_message_id(m);
    EXPECT_EQ(id.size(), 40u);
    EXPECT_EQ(id, sha1_hex("tool  "));  // role + " " + "" + " " + ""
}

// 场景:timestamp 不同 → id 不同(否则 fork 时无法区分两条相同 content 的消息)。
TEST(MessagePayload, DifferentTimestampDifferentId) {
    ChatMessage a;
    a.role = "assistant";
    a.content = "ok";
    a.timestamp = "2026-05-03T10:00:00Z";
    ChatMessage b = a;
    b.timestamp = "2026-05-03T10:00:01Z";
    EXPECT_NE(compute_message_id(a), compute_message_id(b));
}

// 场景:chat_message_to_payload_json 输出含 role / content / id 三个字段,
// id 与 compute_message_id 一致。
TEST(MessagePayload, PayloadJsonContainsId) {
    ChatMessage m;
    m.role = "assistant";
    m.content = "hi";
    m.timestamp = "2026-05-03T10:00:00Z";
    auto j = chat_message_to_payload_json(m);
    ASSERT_TRUE(j.contains("id"));
    EXPECT_EQ(j["id"], compute_message_id(m));
    EXPECT_EQ(j["role"], "assistant");
    EXPECT_EQ(j["content"], "hi");
}

// 场景:user 消息的 payload json 也带 id(等于 uuid)。
TEST(MessagePayload, UserPayloadJsonIdEqualsUuid) {
    ChatMessage m;
    m.role = "user";
    m.content = "x";
    m.uuid = "u-abc";
    auto j = chat_message_to_payload_json(m);
    EXPECT_EQ(j["id"], "u-abc");
}

// 场景:sha1 实现自身的已知向量(RFC 3174 标准用例),保证我们的算法正确。
TEST(MessagePayload, Sha1KnownVectors) {
    EXPECT_EQ(sha1_hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(sha1_hex("The quick brown fox jumps over the lazy dog"),
              "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}
