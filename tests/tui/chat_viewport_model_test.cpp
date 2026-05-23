#include <gtest/gtest.h>

#include "tui/chat_viewport_model.hpp"

#include <vector>

using acecode::tui::ChatViewportState;
using acecode::tui::chat_viewport_build_message_layouts;
using acecode::tui::chat_viewport_clamp_scroll_top_row;
using acecode::tui::chat_viewport_display_row_at;
using acecode::tui::chat_viewport_ensure_row_visible;
using acecode::tui::chat_viewport_is_at_tail;
using acecode::tui::chat_viewport_max_scroll_top_row;
using acecode::tui::chat_viewport_row_for_message_line;
using acecode::tui::chat_viewport_scroll_by_rows;
using acecode::tui::chat_viewport_scroll_to_row;
using acecode::tui::chat_viewport_scroll_to_tail;
using acecode::tui::chat_viewport_set_metrics;
using acecode::tui::chat_viewport_total_rows;
using acecode::tui::chat_viewport_visible_rows;

TEST(ChatViewportModel, EmptyTranscriptHasNoScrollableRows) {
    ChatViewportState state;
    chat_viewport_set_metrics(state, chat_viewport_total_rows({}, 0), 10);

    EXPECT_EQ(state.total_rows, 0);
    EXPECT_EQ(state.scroll_top_row, 0);
    EXPECT_EQ(state.target_scroll_top_row, 0);
    EXPECT_TRUE(state.follow_tail);
    EXPECT_TRUE(chat_viewport_is_at_tail(state));
    EXPECT_TRUE(chat_viewport_visible_rows({}, 0, 0, 10).empty());
}

TEST(ChatViewportModel, ShortTranscriptClampsToTopAndTail) {
    ChatViewportState state;
    chat_viewport_set_metrics(state, chat_viewport_total_rows({1, 2}, 2), 10);

    EXPECT_EQ(state.total_rows, 5);
    EXPECT_EQ(chat_viewport_max_scroll_top_row(state.total_rows,
                                               state.viewport_rows),
              0);
    EXPECT_EQ(chat_viewport_scroll_by_rows(state, -3), 0);
    EXPECT_EQ(chat_viewport_scroll_by_rows(state, 3), 0);
    EXPECT_EQ(state.scroll_top_row, 0);
    EXPECT_TRUE(state.follow_tail);
}

TEST(ChatViewportModel, LongMessageScrollClampsToBounds) {
    ChatViewportState state;
    chat_viewport_set_metrics(state, chat_viewport_total_rows({20}, 1), 5);

    EXPECT_EQ(state.total_rows, 21);
    EXPECT_EQ(state.scroll_top_row, 16);
    EXPECT_EQ(chat_viewport_scroll_by_rows(state, -4), -4);
    EXPECT_EQ(state.scroll_top_row, 12);
    EXPECT_FALSE(state.follow_tail);
    EXPECT_EQ(chat_viewport_scroll_by_rows(state, -99), -12);
    EXPECT_EQ(state.scroll_top_row, 0);
    EXPECT_EQ(chat_viewport_scroll_by_rows(state, 99), 16);
    EXPECT_EQ(state.scroll_top_row, 16);
    EXPECT_TRUE(state.follow_tail);
}

TEST(ChatViewportModel, MessageRowsAndSpacerRowsMapToStableDisplayRows) {
    const std::vector<int> rows = {2, 3};
    EXPECT_EQ(chat_viewport_total_rows(rows, 2), 7);
    EXPECT_EQ(chat_viewport_row_for_message_line(rows, 2, 0, 0), 0);
    EXPECT_EQ(chat_viewport_row_for_message_line(rows, 2, 0, 9), 1);
    EXPECT_EQ(chat_viewport_row_for_message_line(rows, 2, 1, 0), 3);
    EXPECT_EQ(chat_viewport_row_for_message_line(rows, 2, 1, 2), 5);

    auto first_spacer = chat_viewport_display_row_at(rows, 2, 2);
    EXPECT_EQ(first_spacer.message_index, 0);
    EXPECT_EQ(first_spacer.message_row, 1);
    EXPECT_TRUE(first_spacer.spacer_after_message);

    auto second_content = chat_viewport_display_row_at(rows, 2, 4);
    EXPECT_EQ(second_content.message_index, 1);
    EXPECT_EQ(second_content.message_row, 1);
    EXPECT_FALSE(second_content.spacer_after_message);
}

TEST(ChatViewportModel, BuildsMessageLayoutsWithSpacerRows) {
    auto layouts = chat_viewport_build_message_layouts({2, 0, 4}, 3);
    ASSERT_EQ(layouts.size(), 3u);

    EXPECT_EQ(layouts[0].start_row, 0);
    EXPECT_EQ(layouts[0].end_row, 2);
    EXPECT_EQ(layouts[0].spacer_row, 2);

    EXPECT_EQ(layouts[1].row_count, 1);
    EXPECT_EQ(layouts[1].start_row, 3);
    EXPECT_EQ(layouts[1].end_row, 4);
    EXPECT_EQ(layouts[1].spacer_row, 4);

    EXPECT_EQ(layouts[2].start_row, 5);
    EXPECT_EQ(layouts[2].end_row, 9);
    EXPECT_EQ(layouts[2].spacer_row, 9);
}

TEST(ChatViewportModel, ScrollToRowAndTailUpdateFollowTail) {
    ChatViewportState state;
    chat_viewport_set_metrics(state, chat_viewport_total_rows({4, 6}, 2), 5);
    ASSERT_TRUE(state.follow_tail);

    EXPECT_EQ(chat_viewport_scroll_to_row(state, 2), -5);
    EXPECT_EQ(state.scroll_top_row, 2);
    EXPECT_FALSE(state.follow_tail);

    EXPECT_EQ(chat_viewport_scroll_to_tail(state), 5);
    EXPECT_EQ(state.scroll_top_row, 7);
    EXPECT_TRUE(state.follow_tail);
}

TEST(ChatViewportModel, EnsureRowVisibleMovesOnlyWhenNeeded) {
    ChatViewportState state;
    state.scroll_top_row = 4;
    state.follow_tail = false;
    chat_viewport_set_metrics(state, 30, 5);

    EXPECT_EQ(chat_viewport_ensure_row_visible(state, 6), 0);
    EXPECT_EQ(state.scroll_top_row, 4);

    EXPECT_EQ(chat_viewport_ensure_row_visible(state, 3), -1);
    EXPECT_EQ(state.scroll_top_row, 3);

    EXPECT_EQ(chat_viewport_ensure_row_visible(state, 12), 5);
    EXPECT_EQ(state.scroll_top_row, 8);
    EXPECT_FALSE(state.follow_tail);

    EXPECT_EQ(chat_viewport_ensure_row_visible(state, 29), 17);
    EXPECT_EQ(state.scroll_top_row, 25);
    EXPECT_TRUE(state.follow_tail);
}

TEST(ChatViewportModel, VisibleRowsReturnMessageRangesAndSpacers) {
    auto visible = chat_viewport_visible_rows({2, 3, 4}, 3, 1, 5);
    ASSERT_EQ(visible.row_begin, 1);
    ASSERT_EQ(visible.row_end, 6);
    ASSERT_EQ(visible.messages.size(), 2u);

    EXPECT_EQ(visible.messages[0].message_index, 0);
    EXPECT_EQ(visible.messages[0].row_begin, 1);
    EXPECT_EQ(visible.messages[0].row_end, 2);
    EXPECT_TRUE(visible.messages[0].spacer_after_visible);

    EXPECT_EQ(visible.messages[1].message_index, 1);
    EXPECT_EQ(visible.messages[1].row_begin, 0);
    EXPECT_EQ(visible.messages[1].row_end, 3);
    EXPECT_FALSE(visible.messages[1].spacer_after_visible);
}

TEST(ChatViewportModel, VisibleRowsClampTopBottomAndSupportOverscan) {
    auto visible = chat_viewport_visible_rows({5, 5}, 2, 99, 4, 1);

    EXPECT_EQ(visible.row_begin, 7);
    EXPECT_EQ(visible.row_end, 12);
    ASSERT_EQ(visible.messages.size(), 1u);
    EXPECT_EQ(visible.messages[0].message_index, 1);
    EXPECT_EQ(visible.messages[0].row_begin, 1);
    EXPECT_EQ(visible.messages[0].row_end, 5);
    EXPECT_TRUE(visible.messages[0].spacer_after_visible);
}

TEST(ChatViewportModel, WidthRebuildCanClampPreservedReviewPosition) {
    ChatViewportState state;
    state.scroll_top_row = 15;
    state.follow_tail = false;
    chat_viewport_set_metrics(state, chat_viewport_total_rows({12, 12}, 2), 8);
    EXPECT_EQ(state.scroll_top_row, 15);
    EXPECT_FALSE(state.follow_tail);

    chat_viewport_set_metrics(state, chat_viewport_total_rows({3, 3}, 2), 8);
    EXPECT_EQ(state.scroll_top_row, 0);
    EXPECT_TRUE(state.follow_tail);
}
