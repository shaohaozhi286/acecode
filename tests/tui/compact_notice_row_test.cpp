#include "session/compact_notice.hpp"
#include "tui/compact_notice_row.hpp"

#include <gtest/gtest.h>

using acecode::TuiState;

namespace {

acecode::ChatMessage compact_message(const std::string& id,
                                     const std::string& stage,
                                     const std::string& content,
                                     bool complete = false) {
    acecode::ChatMessage message;
    message.role = "system";
    message.content = content;
    message.metadata = acecode::make_compact_notice_metadata(
        id, stage, complete);
    return message;
}

} // namespace

TEST(CompactNoticeRow, KeepsIncompleteOperationExpanded) {
    std::vector<TuiState::Message> rows;
    EXPECT_TRUE(acecode::tui::append_compact_notice_row(
        rows, compact_message("notice-1", "progress", "starting")));
    EXPECT_TRUE(acecode::tui::append_compact_notice_row(
        rows, compact_message("notice-1", "error", "failed")));

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].role, "compact_notice");
    EXPECT_EQ(rows[0].compact_notice_id, "notice-1");
    EXPECT_FALSE(rows[0].compact_notice_complete);
    EXPECT_TRUE(rows[0].expanded);
    EXPECT_EQ(rows[0].content, "starting\n\nfailed");
}

TEST(CompactNoticeRow, CollapsesCompletedOperationWithoutLosingDetails) {
    std::vector<TuiState::Message> rows;
    EXPECT_TRUE(acecode::tui::append_compact_notice_row(
        rows, compact_message("notice-2", "progress", "starting")));
    EXPECT_TRUE(acecode::tui::append_compact_notice_row(
        rows, compact_message("notice-2", "summary", "long summary")));
    EXPECT_TRUE(acecode::tui::append_compact_notice_row(
        rows, compact_message("notice-2", "warning", "warning", true)));

    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].compact_notice_complete);
    EXPECT_FALSE(rows[0].expanded);
    EXPECT_EQ(rows[0].content, "starting\n\nlong summary\n\nwarning");
}

TEST(CompactNoticeRow, LeavesOrdinaryRowsAndDistinctOperationsSeparate) {
    std::vector<TuiState::Message> rows;
    acecode::ChatMessage ordinary;
    ordinary.role = "system";
    ordinary.content = "ordinary";
    EXPECT_FALSE(acecode::tui::append_compact_notice_row(rows, ordinary));
    EXPECT_TRUE(rows.empty());

    EXPECT_TRUE(acecode::tui::append_compact_notice_row(
        rows, compact_message("notice-a", "progress", "a")));
    EXPECT_TRUE(acecode::tui::append_compact_notice_row(
        rows, compact_message("notice-b", "progress", "b")));
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].compact_notice_id, "notice-a");
    EXPECT_EQ(rows[1].compact_notice_id, "notice-b");
}

TEST(CompactNoticeRow, CompletedDisclosureTogglesButIncompleteStaysVisible) {
    TuiState::Message incomplete;
    incomplete.role = "compact_notice";
    incomplete.compact_notice_complete = false;
    incomplete.expanded = false;
    EXPECT_TRUE(acecode::tui::compact_notice_row_is_expanded(
        incomplete, false));
    EXPECT_FALSE(acecode::tui::toggle_completed_compact_notice_row(incomplete));

    TuiState::Message complete;
    complete.role = "compact_notice";
    complete.compact_notice_complete = true;
    EXPECT_FALSE(acecode::tui::compact_notice_row_is_expanded(complete, false));
    EXPECT_TRUE(acecode::tui::compact_notice_row_is_expanded(complete, true));
    EXPECT_TRUE(acecode::tui::toggle_completed_compact_notice_row(complete));
    EXPECT_TRUE(acecode::tui::compact_notice_row_is_expanded(complete, false));
}
