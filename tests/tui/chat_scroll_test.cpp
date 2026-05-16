// 覆盖 FTXUI chat scroll 的纯状态判断:最后一条消息内部不等于真正 tail。
// main.cpp 用这些 helper 决定是否恢复 chat_follow_tail,所以这里不引入 FTXUI。

#include <gtest/gtest.h>

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/screen.hpp"
#include "tui/chat_scroll.hpp"

#include <string>

using acecode::tui::chat_line_count_at;
using acecode::tui::chat_tail_line_offset;
using acecode::tui::chat_bottom_anchor_top_padding_rows;
using acecode::tui::chat_transcript_display_rows;
using acecode::tui::clamp_chat_line_offset;
using acecode::tui::is_chat_tail_position;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Render;
using ftxui::Screen;
using ftxui::emptyElement;
using ftxui::focusPositionRelative;
using ftxui::text;
using ftxui::vbox;
using ftxui::yframe;

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

// 场景: transcript 高度包含每条消息自身高度,以及 main.cpp 在每条消息后
// 固定渲染的空白 spacer 行。
TEST(ChatScroll, TranscriptDisplayRowsIncludeMessageSpacers) {
    EXPECT_EQ(chat_transcript_display_rows({}, 0), 0);
    EXPECT_EQ(chat_transcript_display_rows({3}, 1), 4);
    EXPECT_EQ(chat_transcript_display_rows({2, 0, 5}, 3), 11);
}

// 场景:停在尾部且内容短于 viewport 时,补顶部空白让尾部贴近输入区。
TEST(ChatScroll, TailTopPaddingFillsShortViewportWhenFollowingTail) {
    EXPECT_EQ(chat_bottom_anchor_top_padding_rows({3}, 1, 10), 6);
}

// 场景:内容短于 viewport 时没有实际可滚区域,应贴底;内容已经超过 viewport
// 时不要插入补白,避免破坏滚动语义。
TEST(ChatScroll, TailTopPaddingOnlyFillsShortViewport) {
    EXPECT_EQ(chat_bottom_anchor_top_padding_rows({3}, 1, 10), 6);
    EXPECT_EQ(chat_bottom_anchor_top_padding_rows({9}, 1, 10), 0);
    EXPECT_EQ(chat_bottom_anchor_top_padding_rows({20}, 1, 10), 0);
}

TEST(ChatScroll, FtxuiTailFocusScrollsDocumentToBottom) {
    Elements rows;
    for (int i = 0; i < 10; ++i) {
        rows.push_back(text(std::to_string(i)));
    }
    Element body = vbox(std::move(rows)) | focusPositionRelative(0.0f, 1.0f) | yframe;

    Screen screen(4, 5);
    Render(screen, body);

    EXPECT_EQ(screen.ToString(),
              "5   \r\n"
              "6   \r\n"
              "7   \r\n"
              "8   \r\n"
              "9   ");
}

TEST(ChatScroll, BottomAnchorPaddingMovesShortTranscriptToViewportBottom) {
    Elements rows;
    const int padding =
        chat_bottom_anchor_top_padding_rows({1, 1}, 2, 6);
    for (int i = 0; i < padding; ++i) {
        rows.push_back(text(""));
    }
    rows.push_back(text("0"));
    rows.push_back(text(""));
    rows.push_back(text("1"));
    rows.push_back(text(""));

    Element body = vbox(std::move(rows)) | yframe;
    Screen screen(4, 6);
    Render(screen, body);

    EXPECT_EQ(screen.ToString(),
              "    \r\n"
              "    \r\n"
              "0   \r\n"
              "    \r\n"
              "1   \r\n"
              "    ");
}

TEST(ChatScroll, EmptyOverlayPlaceholdersDoNotReserveRows) {
    Element body = vbox({
        text("chat") | ftxui::flex,
        emptyElement(),
        emptyElement(),
        text("input"),
    });

    Screen screen(8, 4);
    Render(screen, body);

    EXPECT_EQ(screen.ToString(),
              "chat    \r\n"
              "        \r\n"
              "        \r\n"
              "input   ");
}
