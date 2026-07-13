#include <gtest/gtest.h>

#include "tui/chat_message_spacing.hpp"

#include <string>
#include <utility>
#include <vector>

using acecode::TuiState;

namespace {

TuiState::Message message(std::string role) {
    return {std::move(role), "content", false};
}

} // namespace

TEST(ChatMessageSpacing, EmptyHasNoSpacerAndSingleMessageKeepsOriginalSpacer) {
    EXPECT_TRUE(acecode::tui::chat_message_spacer_rows_after({}).empty());
    EXPECT_EQ(acecode::tui::chat_message_spacer_rows_after({message("user")}),
              std::vector<int>({1}));
    EXPECT_EQ(acecode::tui::chat_message_spacer_rows_after({message("system")}),
              std::vector<int>({1}));
}

TEST(ChatMessageSpacing, ConsecutiveStartupNoticesStayCompact) {
    const std::vector<TuiState::Message> messages = {
        message("system"),
        message("system"),
        message("system"),
    };

    EXPECT_EQ(acecode::tui::chat_message_spacer_rows_after(messages),
              std::vector<int>({0, 0, 1}));
}

TEST(ChatMessageSpacing, LastSystemNoticeKeepsSpacerBeforeUser) {
    const std::vector<TuiState::Message> messages = {
        message("system"),
        message("system"),
        message("user"),
    };

    EXPECT_EQ(acecode::tui::chat_message_spacer_rows_after(messages),
              std::vector<int>({0, 1, 1}));
}

TEST(ChatMessageSpacing, AdjacentToolCallAndResultHaveNoSpacer) {
    const std::vector<TuiState::Message> messages = {
        message("user"),
        message("assistant"),
        message("tool_call"),
        message("tool_result"),
        message("system"),
        message("error"),
    };

    EXPECT_EQ(acecode::tui::chat_message_spacer_rows_after(messages),
              std::vector<int>({1, 1, 0, 1, 1, 1}));
}

TEST(ChatMessageSpacing, ToolCallKeepsSpacerBeforeUnrelatedRole) {
    const std::vector<TuiState::Message> messages = {
        message("tool_call"),
        message("assistant"),
        message("tool_call"),
        message("user"),
        message("tool_call"),
        message("error"),
    };

    EXPECT_EQ(acecode::tui::chat_message_spacer_rows_after(messages),
              std::vector<int>({1, 1, 1, 1, 1, 1}));
}

TEST(ChatMessageSpacing, ConversationTurnsKeepOriginalSpacers) {
    const std::vector<TuiState::Message> messages = {
        message("user"),
        message("assistant"),
        message("user"),
        message("assistant"),
        message("user"),
    };

    EXPECT_EQ(acecode::tui::chat_message_spacer_rows_after(messages),
              std::vector<int>({1, 1, 1, 1, 1}));
}
