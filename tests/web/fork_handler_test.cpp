// 覆盖 src/web/handlers/fork_handler.cpp 的纯函数:
//   - compute_fork_title: 命名规则各分支 + UTF-8 截断
//   - find_message_index_by_id: 找到/找不到/重复 ID 取第一个
//
// fork 操作的命名 + 寻址逻辑都在这两个函数里;其它(IO、registry)在 server.cpp
// 由 route handler 胶水调用,不在单测覆盖。

#include <gtest/gtest.h>

#include "web/handlers/fork_handler.hpp"
#include "web/message_payload.hpp"
#include "session/session_storage.hpp"
#include "provider/llm_provider.hpp"

#include <string>
#include <vector>

using acecode::ChatMessage;
using acecode::SessionMeta;
using acecode::web::compute_fork_title;
using acecode::web::compute_message_id;
using acecode::web::find_message_index_by_id;

namespace {
SessionMeta make_meta(const std::string& id, const std::string& title = "",
                       const std::string& summary = "",
                       const std::string& forked_from = "") {
    SessionMeta m;
    m.id = id;
    m.title = title;
    m.summary = summary;
    m.forked_from = forked_from;
    return m;
}
} // namespace

// 场景:无 sibling + 有 source title → "分叉1:<title>"。
TEST(ForkHandler, FirstForkWithTitle) {
    SessionMeta src = make_meta("S", "重构 auth");
    auto title = compute_fork_title(src, {src}, "");
    EXPECT_EQ(title, "分叉1:重构 auth");
}

// 场景:有 1 个 sibling(forked_from == S) → N=2。
TEST(ForkHandler, SiblingCountIncrementsN) {
    SessionMeta src = make_meta("S", "重构 auth");
    SessionMeta first_fork = make_meta("F1", "分叉1:重构 auth", "", "S");
    auto title = compute_fork_title(src, {src, first_fork}, "");
    EXPECT_EQ(title, "分叉2:重构 auth");
}

// 场景:多个 sibling 但只有部分 forked_from == S(其它属于别的源)→ 只数自己的。
TEST(ForkHandler, OnlyOwnSiblingsCounted) {
    SessionMeta src = make_meta("S", "T");
    SessionMeta my1 = make_meta("F1", "x", "", "S");
    SessionMeta my2 = make_meta("F2", "x", "", "S");
    SessionMeta other = make_meta("OF1", "x", "", "OTHER_SRC");
    auto title = compute_fork_title(src, {src, my1, my2, other}, "");
    EXPECT_EQ(title, "分叉3:T");  // 2 个我的 sibling + 1 = 3
}

// 场景:当前 source 是一个已有 fork("分叉1:T")时,新 fork 不应嵌套成
// "分叉1:分叉1:T",而是复用原始标题并避开已有 title → "分叉2:T"。
TEST(ForkHandler, ForkOfForkStripsExistingForkPrefixAndAvoidsCollision) {
    SessionMeta src = make_meta("F1", "分叉1:T", "", "S");
    auto title = compute_fork_title(src, {src}, "");
    EXPECT_EQ(title, "分叉2:T");
}

// 场景:即便 forked_from 不属于当前 source,只要默认标题会撞名,也要递增。
TEST(ForkHandler, TitleCollisionIncrementsEvenForOtherSource) {
    SessionMeta src = make_meta("S", "T");
    SessionMeta other = make_meta("OF1", "分叉1:T", "", "OTHER_SRC");
    auto title = compute_fork_title(src, {src, other}, "");
    EXPECT_EQ(title, "分叉2:T");
}

// 场景:source 没 title 时降级到 summary。
TEST(ForkHandler, FallsBackToSummaryWhenTitleEmpty) {
    SessionMeta src = make_meta("S", "", "user 的第一条消息");
    auto title = compute_fork_title(src, {src}, "");
    EXPECT_EQ(title, "分叉1:user 的第一条消息");
}

// 场景:source title 超过 50 个 codepoint → 截断 + `…`。
// (注:用 ASCII 50 字符确保 codepoint 计数好验证。)
TEST(ForkHandler, LongTitleTruncatedTo50Codepoints) {
    std::string long_title(60, 'a');  // 60 字符,全 ASCII
    SessionMeta src = make_meta("S", long_title);
    auto title = compute_fork_title(src, {src}, "");
    // "分叉1:" 前缀 + 50 a + 省略号
    std::string expected_body = std::string(50, 'a') + "\xE2\x80\xA6";
    EXPECT_EQ(title, "分叉1:" + expected_body);
}

// 场景:中文 title 的 codepoint 截断 — 50 个中文字 = 150 字节 UTF-8,但还是 50 cp。
TEST(ForkHandler, LongCjkTitleTruncatedByCodepoint) {
    std::string cjk;
    for (int i = 0; i < 60; ++i) cjk += "重";  // 每个 3 字节
    SessionMeta src = make_meta("S", cjk);
    auto title = compute_fork_title(src, {src}, "");
    std::string expected_body;
    for (int i = 0; i < 50; ++i) expected_body += "重";
    expected_body += "\xE2\x80\xA6";
    EXPECT_EQ(title, "分叉1:" + expected_body);
}

// 场景:显式 title 直通,不做命名/截断处理。
TEST(ForkHandler, ExplicitTitlePassThrough) {
    SessionMeta src = make_meta("S", "重构 auth");
    auto title = compute_fork_title(src, {src}, "我的自定义 title");
    EXPECT_EQ(title, "我的自定义 title");
}

// 场景:显式 title 全空白(空格 / tab)视作未提供,走自动命名。
TEST(ForkHandler, BlankExplicitTitleFallsBackToAuto) {
    SessionMeta src = make_meta("S", "T");
    auto title = compute_fork_title(src, {src}, "   \t  ");
    EXPECT_EQ(title, "分叉1:T");
}

// 场景:source title + summary 都空 → "分叉N:" 后面没东西(边缘但合法)。
TEST(ForkHandler, EmptyTitleAndSummary) {
    SessionMeta src = make_meta("S");
    auto title = compute_fork_title(src, {src}, "");
    EXPECT_EQ(title, "分叉1:");
}

// 场景:find_message_index_by_id 命中。
TEST(ForkHandler, FindMessageById) {
    std::vector<ChatMessage> msgs(3);
    msgs[0].role = "user"; msgs[0].uuid = "u-1"; msgs[0].content = "a";
    msgs[1].role = "assistant"; msgs[1].content = "b"; msgs[1].timestamp = "ts1";
    msgs[2].role = "user"; msgs[2].uuid = "u-2"; msgs[2].content = "c";

    auto id1 = compute_message_id(msgs[1]);
    auto idx = find_message_index_by_id(msgs, id1);
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 1u);

    auto idx0 = find_message_index_by_id(msgs, "u-1");
    ASSERT_TRUE(idx0.has_value());
    EXPECT_EQ(*idx0, 0u);
}

// 场景:找不到 message_id → nullopt。
TEST(ForkHandler, MessageNotFound) {
    std::vector<ChatMessage> msgs(2);
    msgs[0].role = "user"; msgs[0].uuid = "u-1";
    msgs[1].role = "user"; msgs[1].uuid = "u-2";
    auto idx = find_message_index_by_id(msgs, "does-not-exist");
    EXPECT_FALSE(idx.has_value());
}

// 场景:空 message_id → nullopt(避免回 0 误指第一条)。
TEST(ForkHandler, EmptyIdReturnsNullopt) {
    std::vector<ChatMessage> msgs(1);
    msgs[0].role = "user"; msgs[0].uuid = "u-1";
    auto idx = find_message_index_by_id(msgs, "");
    EXPECT_FALSE(idx.has_value());
}

// 场景:重复 id(理论上罕见,但 spec 说 first wins) — 取第一个。
TEST(ForkHandler, DuplicateIdReturnsFirst) {
    // 构造两条同 role 同 content 同 ts 的 assistant 消息 → sha1 相同
    std::vector<ChatMessage> msgs(2);
    msgs[0].role = "assistant"; msgs[0].content = "ok"; msgs[0].timestamp = "ts";
    msgs[1].role = "assistant"; msgs[1].content = "ok"; msgs[1].timestamp = "ts";
    auto id = compute_message_id(msgs[0]);
    EXPECT_EQ(id, compute_message_id(msgs[1]));  // 前提:确实撞 id
    auto idx = find_message_index_by_id(msgs, id);
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(*idx, 0u);  // first wins
}
