#include <gtest/gtest.h>

#include "tui/chat_line_measure.hpp"

using acecode::tui::ChatLineMeasure;
using acecode::tui::chat_line_counts_from_measures;
using acecode::tui::chat_line_measure_rows;
using acecode::tui::invalidate_chat_line_measure;
using acecode::tui::invalidate_chat_line_measures;
using acecode::tui::resize_chat_line_measures;
using acecode::tui::sync_chat_line_measure;

TEST(ChatLineMeasure, ValidLayoutMeasurementCanShrinkPreviousEstimate) {
    ChatLineMeasure measure;

    EXPECT_TRUE(sync_chat_line_measure(measure, true, 1004, 120, 1));
    EXPECT_EQ(chat_line_measure_rows(measure), 1004);
    EXPECT_TRUE(measure.valid);

    EXPECT_TRUE(sync_chat_line_measure(measure, true, 1, 120, 2));
    EXPECT_EQ(chat_line_measure_rows(measure), 1);
    EXPECT_TRUE(measure.valid);
}

TEST(ChatLineMeasure, ClippedOrMissingMeasurementDoesNotShrinkStableEstimate) {
    ChatLineMeasure measure;
    ASSERT_TRUE(sync_chat_line_measure(measure, true, 20, 80, 7));

    EXPECT_FALSE(sync_chat_line_measure(measure, false, 5, 80, 7));
    EXPECT_EQ(chat_line_measure_rows(measure), 20);
    EXPECT_TRUE(measure.valid);
}

TEST(ChatLineMeasure, InvalidatedMessageUsesFallbackUntilMeasuredAgain) {
    std::vector<ChatLineMeasure> measures(1);
    ASSERT_TRUE(sync_chat_line_measure(measures[0], true, 50, 80, 3));

    invalidate_chat_line_measure(measures, 0);
    EXPECT_FALSE(measures[0].valid);
    EXPECT_EQ(chat_line_measure_rows(measures[0]), 1);

    EXPECT_FALSE(sync_chat_line_measure(measures[0], false, 0, 80, 4));
    EXPECT_EQ(chat_line_measure_rows(measures[0]), 1);
    EXPECT_FALSE(measures[0].valid);

    EXPECT_TRUE(sync_chat_line_measure(measures[0], true, 4, 80, 4));
    EXPECT_EQ(chat_line_measure_rows(measures[0]), 4);
    EXPECT_TRUE(measures[0].valid);
}

TEST(ChatLineMeasure, WidthChangeInvalidatesWrappingSensitiveEstimate) {
    ChatLineMeasure measure;
    ASSERT_TRUE(sync_chat_line_measure(measure, true, 10, 100, 1));

    EXPECT_FALSE(sync_chat_line_measure(measure, false, 0, 60, 1));
    EXPECT_EQ(chat_line_measure_rows(measure), 1);
    EXPECT_FALSE(measure.valid);
}

TEST(ChatLineMeasure, CountsFallbackForMissingAndInvalidMeasures) {
    std::vector<ChatLineMeasure> measures;
    resize_chat_line_measures(measures, 3);
    ASSERT_TRUE(sync_chat_line_measure(measures[0], true, 2, 80, 1));
    ASSERT_TRUE(sync_chat_line_measure(measures[1], true, 5, 80, 2));
    ASSERT_TRUE(sync_chat_line_measure(measures[2], true, 8, 80, 3));

    invalidate_chat_line_measures(measures);
    auto counts = chat_line_counts_from_measures(measures, 4);

    ASSERT_EQ(counts.size(), 4u);
    EXPECT_EQ(counts[0], 1);
    EXPECT_EQ(counts[1], 1);
    EXPECT_EQ(counts[2], 1);
    EXPECT_EQ(counts[3], 1);
}
