#include <gtest/gtest.h>

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/screen.hpp"
#include "tui/chat_line_measure.hpp"
#include "tui/chat_render_window.hpp"

using acecode::tui::ChatLineMeasure;
using acecode::tui::chat_render_window;
using acecode::tui::chat_line_counts_from_measures;
using acecode::tui::default_chat_render_overscan_rows;
using acecode::tui::full_chat_render_window;
using acecode::tui::invalidate_chat_line_measure;
using acecode::tui::sync_chat_line_measure;
using ftxui::EQUAL;
using ftxui::Element;
using ftxui::HEIGHT;
using ftxui::Render;
using ftxui::Screen;
using ftxui::emptyElement;
using ftxui::size;
using ftxui::text;
using ftxui::vbox;

TEST(ChatRenderWindow, EmptyTranscriptHasEmptyWindow) {
    auto window = chat_render_window({}, 0, 0, 10, 20);

    EXPECT_EQ(window.first_message, 0);
    EXPECT_EQ(window.last_message_exclusive, 0);
    EXPECT_EQ(window.top_spacer_rows, 0);
    EXPECT_EQ(window.bottom_spacer_rows, 0);
    EXPECT_EQ(window.total_rows, 0);
}

TEST(ChatRenderWindow, FullWindowCoversAllMessages) {
    auto window = full_chat_render_window({2, 3}, 2);

    EXPECT_EQ(window.first_message, 0);
    EXPECT_EQ(window.last_message_exclusive, 2);
    EXPECT_EQ(window.top_spacer_rows, 0);
    EXPECT_EQ(window.bottom_spacer_rows, 0);
    EXPECT_EQ(window.total_rows, 7);
}

TEST(ChatRenderWindow, NonPositiveViewportFallsBackToFullWindow) {
    auto window = chat_render_window({3, 3, 3}, 3, 8, 0, 4);

    EXPECT_EQ(window.first_message, 0);
    EXPECT_EQ(window.last_message_exclusive, 3);
    EXPECT_EQ(window.top_spacer_rows, 0);
    EXPECT_EQ(window.bottom_spacer_rows, 0);
    EXPECT_EQ(window.total_rows, 12);
    EXPECT_EQ(window.overscan_rows, 4);
}

TEST(ChatRenderWindow, FullyVisibleTranscriptHasNoSpacers) {
    auto window = chat_render_window({2, 3}, 2, 0, 20, 0);

    EXPECT_EQ(window.first_message, 0);
    EXPECT_EQ(window.last_message_exclusive, 2);
    EXPECT_EQ(window.top_spacer_rows, 0);
    EXPECT_EQ(window.bottom_spacer_rows, 0);
    EXPECT_EQ(window.total_rows, 7);
}

TEST(ChatRenderWindow, MiddleViewportRendersOnlyIntersectingMessage) {
    auto window = chat_render_window({3, 3, 3, 3, 3}, 5, 8, 4, 0);

    EXPECT_EQ(window.first_message, 2);
    EXPECT_EQ(window.last_message_exclusive, 3);
    EXPECT_EQ(window.top_spacer_rows, 8);
    EXPECT_EQ(window.bottom_spacer_rows, 8);
    EXPECT_EQ(window.total_rows, 20);
}

TEST(ChatRenderWindow, OverscanIncludesNearbyMessages) {
    auto window = chat_render_window({3, 3, 3, 3, 3}, 5, 8, 4, 4);

    EXPECT_EQ(window.first_message, 1);
    EXPECT_EQ(window.last_message_exclusive, 4);
    EXPECT_EQ(window.top_spacer_rows, 4);
    EXPECT_EQ(window.bottom_spacer_rows, 4);
    EXPECT_EQ(window.overscan_rows, 4);
}

TEST(ChatRenderWindow, TopViewportClampsToFirstMessage) {
    auto window = chat_render_window({3, 3, 3, 3, 3}, 5, -50, 4, 0);

    EXPECT_EQ(window.first_message, 0);
    EXPECT_EQ(window.last_message_exclusive, 1);
    EXPECT_EQ(window.top_spacer_rows, 0);
    EXPECT_EQ(window.bottom_spacer_rows, 16);
}

TEST(ChatRenderWindow, BottomViewportClampsToLastMessage) {
    auto window = chat_render_window({3, 3, 3, 3, 3}, 5, 999, 4, 0);

    EXPECT_EQ(window.first_message, 4);
    EXPECT_EQ(window.last_message_exclusive, 5);
    EXPECT_EQ(window.top_spacer_rows, 16);
    EXPECT_EQ(window.bottom_spacer_rows, 0);
}

TEST(ChatRenderWindow, MissingAndZeroLineCountsAreEstimatedAsOneLine) {
    auto window = chat_render_window({0}, 3, 2, 2, 0);

    EXPECT_EQ(window.total_rows, 6);
    EXPECT_EQ(window.first_message, 1);
    EXPECT_EQ(window.last_message_exclusive, 2);
    EXPECT_EQ(window.top_spacer_rows, 2);
    EXPECT_EQ(window.bottom_spacer_rows, 2);
}

TEST(ChatRenderWindow, DefaultOverscanUsesTwoViewportsWithMinimum) {
    EXPECT_EQ(default_chat_render_overscan_rows(0), 0);
    EXPECT_EQ(default_chat_render_overscan_rows(5), 24);
    EXPECT_EQ(default_chat_render_overscan_rows(20), 40);
}

TEST(ChatRenderWindow, FoldedToolResultShrinkRemovesStaleSpacerGap) {
    std::vector<ChatLineMeasure> measures(1);
    ASSERT_TRUE(sync_chat_line_measure(measures[0], true, 1000, 100, 1));

    invalidate_chat_line_measure(measures, 0);
    ASSERT_TRUE(sync_chat_line_measure(measures[0], true, 1, 100, 2));
    auto counts = chat_line_counts_from_measures(measures, 1);

    auto window = chat_render_window(counts, 1, 500, 20, 0);

    EXPECT_EQ(window.total_rows, 2);
    EXPECT_EQ(window.first_message, 0);
    EXPECT_EQ(window.last_message_exclusive, 1);
    EXPECT_EQ(window.top_spacer_rows, 0);
    EXPECT_EQ(window.bottom_spacer_rows, 0);
}

TEST(ChatRenderWindow, FtxuiFixedHeightSpacerPreservesRows) {
    Element body = vbox({
        text("top"),
        emptyElement() | size(HEIGHT, EQUAL, 3),
        text("bottom"),
    });

    Screen screen(8, 5);
    Render(screen, body);

    EXPECT_EQ(screen.ToString(),
              "top     \r\n"
              "        \r\n"
              "        \r\n"
              "        \r\n"
              "bottom  ");
}
