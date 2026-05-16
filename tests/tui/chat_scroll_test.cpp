// 覆盖 FTXUI chat scroll 的纯状态判断:最后一条消息内部不等于真正 tail。
// main.cpp 用这些 helper 决定是否恢复 chat_follow_tail,所以这里不引入 FTXUI。

#include <gtest/gtest.h>

#include "tui/chat_scroll.hpp"

using acecode::tui::chat_line_count_at;
using acecode::tui::chat_tail_line_offset;
using acecode::tui::clamp_chat_line_offset;
using acecode::tui::is_chat_tail_position;

// 场景:空会话没有可聚焦消息,不应被判断为位于 tail。
TEST(ChatScroll, EmptyChatIsNotTailPosition) {
    EXPECT_FALSE(is_chat_tail_position(-1, 0, 0, {}));
}

// 场景:缺失或退化的 line_count 按 1 行处理,匹配 main.cpp 原来的 fallback。
TEST(ChatScroll, MissingAndZeroLineCountsAreOneLine) {
    EXPECT_EQ(chat_line_count_at({}, 0), 1);
    EXPECT_EQ(chat_line_count_at({0}, 0), 1);
    EXPECT_EQ(chat_tail_line_offset({0}, 0), 0);
}

// 场景:短消息只有一行,最后一条消息 offset 0 就是 tail。
TEST(ChatScroll, SingleLineLastMessageAtOffsetZeroIsTail) {
    EXPECT_TRUE(is_chat_tail_position(1, 0, 2, {1, 1}));
}

// 场景:长的最后一条 assistant 消息内部不是 tail,即使 focus_index 已经是最后一条。
TEST(ChatScroll, InteriorLineOfLongLastMessageIsNotTail) {
    EXPECT_FALSE(is_chat_tail_position(2, 4, 3, {1, 2, 20}));
}

// 场景:长的最后一条消息最后一行才恢复 tail-follow。
TEST(ChatScroll, FinalLineOfLongLastMessageIsTail) {
    EXPECT_TRUE(is_chat_tail_position(2, 19, 3, {1, 2, 20}));
    EXPECT_TRUE(is_chat_tail_position(2, 25, 3, {1, 2, 20}));
}

// 场景:不是最后一条消息时,即使 offset 很大也不是 tail。
TEST(ChatScroll, NonLastMessageIsNeverTail) {
    EXPECT_FALSE(is_chat_tail_position(1, 99, 3, {1, 2, 20}));
}

// 场景:line offset clamp 始终落在当前消息有效行范围内。
TEST(ChatScroll, ClampLineOffsetToMessageBounds) {
    EXPECT_EQ(clamp_chat_line_offset(-3, 5), 0);
    EXPECT_EQ(clamp_chat_line_offset(2, 5), 2);
    EXPECT_EQ(clamp_chat_line_offset(9, 5), 4);
    EXPECT_EQ(clamp_chat_line_offset(9, 0), 0);
}
