#include "tui/pending_attachment_selection.hpp"

#include <gtest/gtest.h>

namespace {

using acecode::tui::clamp_pending_attachment_focus;
using acecode::tui::has_pending_attachment_focus;
using acecode::tui::kNoPendingAttachmentFocus;
using acecode::tui::move_pending_attachment_focus;
using acecode::tui::remove_focused_pending_attachment_index;
using acecode::tui::toggle_pending_attachment_focus;

TEST(PendingAttachmentSelection, ToggleFocusSelectsLastAttachment) {
    int selected = kNoPendingAttachmentFocus;

    EXPECT_TRUE(toggle_pending_attachment_focus(selected, 3));
    EXPECT_EQ(selected, 2);
    EXPECT_TRUE(has_pending_attachment_focus(selected, 3));

    EXPECT_TRUE(toggle_pending_attachment_focus(selected, 3));
    EXPECT_EQ(selected, kNoPendingAttachmentFocus);
}

TEST(PendingAttachmentSelection, ToggleWithNoAttachmentsClearsFocus) {
    int selected = 2;

    EXPECT_TRUE(toggle_pending_attachment_focus(selected, 0));
    EXPECT_EQ(selected, kNoPendingAttachmentFocus);
}

TEST(PendingAttachmentSelection, MoveClampsAtListEdges) {
    int selected = 1;

    EXPECT_TRUE(move_pending_attachment_focus(selected, 3, -1));
    EXPECT_EQ(selected, 0);
    EXPECT_FALSE(move_pending_attachment_focus(selected, 3, -1));
    EXPECT_EQ(selected, 0);

    EXPECT_TRUE(move_pending_attachment_focus(selected, 3, 9));
    EXPECT_EQ(selected, 2);
    EXPECT_FALSE(move_pending_attachment_focus(selected, 3, 1));
    EXPECT_EQ(selected, 2);
}

TEST(PendingAttachmentSelection, MoveWithoutFocusDoesNotEnterFocus) {
    int selected = kNoPendingAttachmentFocus;

    EXPECT_FALSE(move_pending_attachment_focus(selected, 2, 1));
    EXPECT_EQ(selected, kNoPendingAttachmentFocus);
}

TEST(PendingAttachmentSelection, RemoveFocusedAttachmentKeepsNearbySelection) {
    int selected = 1;

    auto removed = remove_focused_pending_attachment_index(selected, 3);

    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, 1u);
    EXPECT_EQ(selected, 1);
}

TEST(PendingAttachmentSelection, RemoveLastAttachmentMovesSelectionBackward) {
    int selected = 2;

    auto removed = remove_focused_pending_attachment_index(selected, 3);

    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, 2u);
    EXPECT_EQ(selected, 1);
}

TEST(PendingAttachmentSelection, RemoveOnlyAttachmentClearsFocus) {
    int selected = 0;

    auto removed = remove_focused_pending_attachment_index(selected, 1);

    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, 0u);
    EXPECT_EQ(selected, kNoPendingAttachmentFocus);
}

TEST(PendingAttachmentSelection, ClampClearsOrBoundsFocus) {
    int selected = 7;
    EXPECT_TRUE(clamp_pending_attachment_focus(selected, 2));
    EXPECT_EQ(selected, 1);

    EXPECT_TRUE(clamp_pending_attachment_focus(selected, 0));
    EXPECT_EQ(selected, kNoPendingAttachmentFocus);
}

} // namespace
