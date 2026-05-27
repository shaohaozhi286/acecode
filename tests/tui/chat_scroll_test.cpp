// 覆盖 FTXUI chat scroll 的纯状态判断:最后一条消息内部不等于真正 tail。
// main.cpp 用这些 helper 决定是否恢复 chat_follow_tail,所以这里不引入 FTXUI。

#include <gtest/gtest.h>

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/screen.hpp"
#include "tui/chat_scroll.hpp"
#include "tui/unclipped_reflect.hpp"

#include <string>
#include <utility>

using acecode::tui::chat_line_count_at;
using acecode::tui::chat_tail_line_offset;
using acecode::tui::chat_bottom_anchor_top_padding_rows;
using acecode::tui::chat_display_row_for_focus;
using acecode::tui::chat_focus_from_display_row;
using acecode::tui::chat_frame_focus_y_for_scroll_top;
using acecode::tui::chat_max_scroll_top_row;
using acecode::tui::chat_scrollbar_grab_offset_2x;
using acecode::tui::chat_scrollbar_thumb_geometry;
using acecode::tui::chat_scrollbar_y_to_top_row;
using acecode::tui::chat_scrollbar_y_to_top_row_with_grab;
using acecode::tui::chat_transcript_display_rows;
using acecode::tui::clamp_chat_scroll_top_row;
using acecode::tui::clamp_chat_line_offset;
using acecode::tui::is_chat_tail_position;
using acecode::tui::is_chat_mouse_target;
using acecode::tui::update_chat_line_count_estimate;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Render;
using ftxui::Screen;
using ftxui::emptyElement;
using ftxui::focusPosition;
using ftxui::focusPositionRelative;
using ftxui::reflect;
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

TEST(ChatScroll, LineCountEstimateDoesNotShrinkFromClippedReflectBox) {
    EXPECT_EQ(update_chat_line_count_estimate(0, 12), 12);
    EXPECT_EQ(update_chat_line_count_estimate(12, 8), 12);
    EXPECT_EQ(update_chat_line_count_estimate(12, 15), 15);
    EXPECT_EQ(update_chat_line_count_estimate(12, 0), 12);
}

TEST(ChatScroll, UnclippedReflectKeepsFullFrameLayoutHeight) {
    ftxui::Box clipped_box;
    ftxui::Box layout_box;

    Elements rows;
    for (int i = 0; i < 20; ++i) {
        rows.push_back(text(std::to_string(i)));
    }

    Element block = vbox(std::move(rows))
        | acecode::tui::reflect_unclipped(layout_box)
        | reflect(clipped_box);
    Element body = vbox({block})
        | focusPosition(0, chat_frame_focus_y_for_scroll_top(15, 5))
        | yframe;

    Screen screen(4, 5);
    Render(screen, body);

    EXPECT_EQ(layout_box.y_max - layout_box.y_min + 1, 20);
    EXPECT_LT(clipped_box.y_max - clipped_box.y_min + 1, 20);
    EXPECT_EQ(screen.ToString(),
              "15  \r\n"
              "16  \r\n"
              "17  \r\n"
              "18  \r\n"
              "19  ");
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

TEST(ChatScroll, DisplayRowsIncludeSpacersForFocusMapping) {
    EXPECT_EQ(chat_display_row_for_focus({2, 3}, 2, 0, 0), 0);
    EXPECT_EQ(chat_display_row_for_focus({2, 3}, 2, 0, 1), 1);
    EXPECT_EQ(chat_display_row_for_focus({2, 3}, 2, 1, 0), 3);
    EXPECT_EQ(chat_display_row_for_focus({2, 3}, 2, 1, 2), 5);
}

TEST(ChatScroll, FocusMappingTreatsSpacerAsPreviousMessageTail) {
    EXPECT_EQ(chat_focus_from_display_row({2, 3}, 2, 0), std::make_pair(0, 0));
    EXPECT_EQ(chat_focus_from_display_row({2, 3}, 2, 1), std::make_pair(0, 1));
    EXPECT_EQ(chat_focus_from_display_row({2, 3}, 2, 2), std::make_pair(0, 1));
    EXPECT_EQ(chat_focus_from_display_row({2, 3}, 2, 3), std::make_pair(1, 0));
    EXPECT_EQ(chat_focus_from_display_row({2, 3}, 2, 6), std::make_pair(1, 2));
}

TEST(ChatScroll, ScrollTopClampsToTranscriptRange) {
    EXPECT_EQ(chat_max_scroll_top_row({4, 3}, 2, 5), 4);
    EXPECT_EQ(clamp_chat_scroll_top_row(-2, {4, 3}, 2, 5), 0);
    EXPECT_EQ(clamp_chat_scroll_top_row(2, {4, 3}, 2, 5), 2);
    EXPECT_EQ(clamp_chat_scroll_top_row(99, {4, 3}, 2, 5), 4);
}

TEST(ChatScroll, ScrollbarMapsMouseYToViewportTop) {
    EXPECT_EQ(chat_scrollbar_y_to_top_row(10, 10, 5, {10, 10}, 2, 6), 0);
    EXPECT_EQ(chat_scrollbar_y_to_top_row(14, 10, 5, {10, 10}, 2, 6), 16);
    EXPECT_EQ(chat_scrollbar_y_to_top_row(12, 10, 5, {10, 10}, 2, 6), 8);
}

TEST(ChatScroll, ScrollbarDragPreservesGrabOffsetToReachBottom) {
    const std::vector<int> lines = {200};
    const int message_count = 1;
    const int viewport_rows = 40;
    const int track_y_min = 0;
    const int track_height = 40;
    const int max_top =
        chat_max_scroll_top_row(lines, message_count, viewport_rows);

    auto geometry = chat_scrollbar_thumb_geometry(
        track_y_min, track_height, lines, message_count, viewport_rows,
        /*scroll_top_row=*/0);
    const int grab_offset =
        chat_scrollbar_grab_offset_2x(/*mouse_y=*/4, geometry);

    EXPECT_LT(chat_scrollbar_y_to_top_row(/*mouse_y=*/37, track_y_min,
                                          track_height, lines, message_count,
                                          viewport_rows),
              max_top);
    EXPECT_EQ(chat_scrollbar_y_to_top_row_with_grab(/*mouse_y=*/37,
                                                    track_y_min, geometry,
                                                    grab_offset),
              max_top);
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

TEST(ChatScroll, TailFollowRelativeFocusIgnoresStaleAbsoluteTop) {
    auto render_with_stale_absolute_top = [] {
        Elements rows;
        for (int i = 0; i < 20; ++i) {
            rows.push_back(text(std::to_string(i)));
        }
        Element body = vbox(std::move(rows))
            | focusPosition(0, chat_frame_focus_y_for_scroll_top(0, 5))
            | yframe;

        Screen screen(4, 5);
        Render(screen, body);
        return screen.ToString();
    };

    auto render_with_tail_anchor = [] {
        Elements rows;
        for (int i = 0; i < 20; ++i) {
            rows.push_back(text(std::to_string(i)));
        }
        Element body = vbox(std::move(rows))
            | focusPositionRelative(0.0f, 1.0f)
            | yframe;

        Screen screen(4, 5);
        Render(screen, body);
        return screen.ToString();
    };

    EXPECT_NE(render_with_stale_absolute_top(),
              "15  \r\n"
              "16  \r\n"
              "17  \r\n"
              "18  \r\n"
              "19  ");
    EXPECT_EQ(render_with_tail_anchor(),
              "15  \r\n"
              "16  \r\n"
              "17  \r\n"
              "18  \r\n"
              "19  ");
}

TEST(ChatScroll, AbsoluteScrollTopMovesOneRowAtViewportEdges) {
    auto render_at_top = [](int scroll_top) {
        Elements rows;
        for (int i = 0; i < 20; ++i) {
            rows.push_back(text(std::to_string(i)));
        }
        const int viewport_rows = 5;
        Element body = vbox(std::move(rows))
            | focusPosition(0, chat_frame_focus_y_for_scroll_top(scroll_top,
                                                                  viewport_rows))
            | yframe;

        Screen screen(4, viewport_rows);
        Render(screen, body);
        return screen.ToString();
    };

    EXPECT_EQ(render_at_top(15),
              "15  \r\n"
              "16  \r\n"
              "17  \r\n"
              "18  \r\n"
              "19  ");
    EXPECT_EQ(render_at_top(14),
              "14  \r\n"
              "15  \r\n"
              "16  \r\n"
              "17  \r\n"
              "18  ");
    EXPECT_EQ(render_at_top(0),
              "0   \r\n"
              "1   \r\n"
              "2   \r\n"
              "3   \r\n"
              "4   ");
    EXPECT_EQ(render_at_top(1),
              "1   \r\n"
              "2   \r\n"
              "3   \r\n"
              "4   \r\n"
              "5   ");
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

TEST(ChatScroll, WheelAboveChatIsAcceptedForTerminalOriginMismatch) {
    EXPECT_TRUE(is_chat_mouse_target(50, 2, 1, 5, 118, 25, true));
}

TEST(ChatScroll, NonWheelAboveChatIsRejected) {
    EXPECT_FALSE(is_chat_mouse_target(50, 2, 1, 5, 118, 25, false));
}

TEST(ChatScroll, WheelBelowChatIsRejected) {
    EXPECT_FALSE(is_chat_mouse_target(50, 26, 1, 5, 118, 25, true));
}

TEST(ChatScroll, DegenerateChatBoxIsRejected) {
    EXPECT_FALSE(is_chat_mouse_target(50, 2, 1, 5, 118, 4, true));
}
