#include <gtest/gtest.h>

#include "desktop/tray_menu_popup_model.hpp"

#include <string>
#include <vector>

namespace acecode::desktop {
namespace {

TrayMenuItem popup_item(const std::string& id, const std::string& workspace) {
    TrayMenuItem item;
    item.session_id = id;
    item.workspace_hash = workspace;
    item.title = "Session " + id;
    item.subtitle = workspace;
    return item;
}

std::vector<TrayMenuEntryKind> popup_kinds(const std::vector<TrayPopupRow>& rows) {
    std::vector<TrayMenuEntryKind> kinds;
    for (const auto& row : rows) kinds.push_back(row.entry.kind);
    return kinds;
}

TEST(TrayMenuPopupModel, AttachesEachOverflowGroupToItsOwnMoreRow) {
    TrayMenuPayload payload;
    for (int i = 0; i < 5; ++i) {
        payload.pinned.push_back(popup_item("p" + std::to_string(i), "pinned"));
    }
    for (int i = 0; i < 7; ++i) {
        payload.recent.push_back(popup_item("r" + std::to_string(i), "recent"));
    }

    const auto rows = build_tray_popup_rows(compute_menu_layout(payload));
    std::vector<const TrayPopupRow*> more_rows;
    for (const auto& row : rows) {
        if (row.entry.kind == TrayMenuEntryKind::MoreSubmenuRoot) {
            more_rows.push_back(&row);
        }
        EXPECT_NE(row.entry.kind, TrayMenuEntryKind::MoreSubmenuItem);
    }

    ASSERT_EQ(more_rows.size(), 2u);
    ASSERT_EQ(more_rows[0]->submenu_entries.size(), 2u);
    EXPECT_EQ(more_rows[0]->submenu_entries[0].session_id, "p3");
    EXPECT_EQ(more_rows[0]->submenu_entries[1].session_id, "p4");
    ASSERT_EQ(more_rows[1]->submenu_entries.size(), 4u);
    EXPECT_EQ(more_rows[1]->submenu_entries.front().session_id, "r3");
    EXPECT_EQ(more_rows[1]->submenu_entries.back().session_id, "r6");
}

TEST(TrayMenuPopupModel, SelectabilityMatchesInteractiveRows) {
    EXPECT_FALSE(tray_popup_entry_is_selectable(TrayMenuEntryKind::PinnedHeader));
    EXPECT_FALSE(tray_popup_entry_is_selectable(TrayMenuEntryKind::RecentHeader));
    EXPECT_FALSE(tray_popup_entry_is_selectable(TrayMenuEntryKind::Separator));

    EXPECT_TRUE(tray_popup_entry_is_selectable(TrayMenuEntryKind::PinnedItem));
    EXPECT_TRUE(tray_popup_entry_is_selectable(TrayMenuEntryKind::RecentItem));
    EXPECT_TRUE(tray_popup_entry_is_selectable(TrayMenuEntryKind::MoreSubmenuRoot));
    EXPECT_TRUE(tray_popup_entry_is_selectable(TrayMenuEntryKind::MoreSubmenuItem));
    EXPECT_TRUE(tray_popup_entry_is_selectable(TrayMenuEntryKind::NewChat));
    EXPECT_TRUE(tray_popup_entry_is_selectable(TrayMenuEntryKind::OpenApp));
    EXPECT_TRUE(tray_popup_entry_is_selectable(TrayMenuEntryKind::Quit));
}

TEST(TrayMenuPopupModel, PreservesFixedActionOrdering) {
    TrayMenuPayload payload;
    payload.pinned.push_back(popup_item("p0", "pinned"));
    const auto rows = build_tray_popup_rows(compute_menu_layout(payload));
    const auto kinds = popup_kinds(rows);

    ASSERT_GE(kinds.size(), 5u);
    EXPECT_EQ(kinds[kinds.size() - 5], TrayMenuEntryKind::NewChat);
    EXPECT_EQ(kinds[kinds.size() - 4], TrayMenuEntryKind::Separator);
    EXPECT_EQ(kinds[kinds.size() - 3], TrayMenuEntryKind::OpenApp);
    EXPECT_EQ(kinds[kinds.size() - 2], TrayMenuEntryKind::Separator);
    EXPECT_EQ(kinds[kinds.size() - 1], TrayMenuEntryKind::Quit);
}

TEST(TrayMenuPopupModel, SubmenuRowsKeepSessionCommandMetadata) {
    TrayMenuPayload payload;
    for (int i = 0; i < 5; ++i) {
        payload.recent.push_back(popup_item("r" + std::to_string(i), "workspace"));
    }
    const auto rows = build_tray_popup_rows(compute_menu_layout(payload));

    const TrayPopupRow* root = nullptr;
    for (const auto& row : rows) {
        if (tray_popup_row_has_submenu(row)) root = &row;
    }
    ASSERT_NE(root, nullptr);

    const auto submenu = build_tray_popup_submenu_rows(*root);
    ASSERT_EQ(submenu.size(), 2u);
    EXPECT_EQ(submenu[0].entry.session_id, "r3");
    EXPECT_EQ(submenu[1].entry.session_id, "r4");
    EXPECT_GT(submenu[0].entry.id, 0u);
    EXPECT_TRUE(tray_popup_entry_is_selectable(submenu[0].entry.kind));
}

} // namespace
} // namespace acecode::desktop
