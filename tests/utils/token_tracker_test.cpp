#include <gtest/gtest.h>

#include "utils/token_tracker.hpp"

TEST(TokenTracker, ContextPercentRoundsAndClamps) {
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(0, 128000), 0);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(10240, 128000), 8);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(51200, 128000), 40);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(128001, 128000), 100);
    EXPECT_EQ(acecode::TokenTracker::context_percent_for(1000, 0), 0);
}

TEST(TokenTracker, ContextPercentUsesLastPromptTokens) {
    acecode::TokenTracker tracker;
    acecode::TokenUsage usage;
    usage.prompt_tokens = 76800;
    usage.total_tokens = 80000;
    usage.has_data = true;

    tracker.record(usage);

    EXPECT_EQ(tracker.context_percent(128000), 60);
}
