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

TEST(TrayMenuPopupModel, BottomRightCornerMatchesCursorInsideFullMonitorBounds) {
    const auto position = compute_tray_popup_position(
        598,
        683,
        560,
        589,
        0,
        0,
        1077,
        691);

    EXPECT_EQ(position.x + 560, 598);
    EXPECT_EQ(position.y + 589, 683);
}

TEST(TrayMenuPopupModel, FlipsAtTopLeftWithoutMovingCursorAnchor) {
    const auto position = compute_tray_popup_position(
        20,
        20,
        280,
        300,
        0,
        0,
        1077,
        691);

    EXPECT_EQ(position.x, 20);
    EXPECT_EQ(position.y, 20);
}

TEST(TrayMenuPopupModel, MonitorScaleConvertsToGeometryDpi) {
    EXPECT_EQ(
        compute_tray_popup_geometry_dpi_from_monitor_scale_percent(100),
        96);
    EXPECT_EQ(
        compute_tray_popup_geometry_dpi_from_monitor_scale_percent(125),
        120);
    EXPECT_EQ(
        compute_tray_popup_geometry_dpi_from_monitor_scale_percent(150),
        144);
}

TEST(TrayMenuPopupModel, InvalidMonitorScaleFallsBackToCompactGeometry) {
    EXPECT_EQ(
        compute_tray_popup_geometry_dpi_from_monitor_scale_percent(0),
        96);
    EXPECT_EQ(
        compute_tray_popup_geometry_dpi_from_monitor_scale_percent(99),
        96);
    EXPECT_EQ(
        compute_tray_popup_geometry_dpi_from_monitor_scale_percent(501),
        96);
}

TEST(TrayMenuPopupModel, MonitorScaleChangesMenuWidthExactlyOnce) {
    EXPECT_EQ(scale_tray_popup_size_px(280, 96), 280);
    EXPECT_EQ(scale_tray_popup_size_px(280, 120), 350);
    EXPECT_EQ(scale_tray_popup_size_px(280, 144), 420);
}

TEST(TrayMenuPopupModel, DefaultTextScaleKeepsFontAtItsPixelDesignHeight) {
    EXPECT_EQ(compute_tray_popup_font_height_px(13, 100), 13);
}

TEST(TrayMenuPopupModel, EnlargedTextScaleRoundsFontHeightToNearestPixel) {
    EXPECT_EQ(compute_tray_popup_font_height_px(13, 125), 16);
    EXPECT_EQ(compute_tray_popup_font_height_px(13, 150), 20);
    EXPECT_EQ(compute_tray_popup_font_height_px(13, 225), 29);
}

TEST(TrayMenuPopupModel, UnsupportedTextScaleFallsBackToOneHundredPercent) {
    EXPECT_EQ(normalize_tray_popup_text_scale_percent(0), 100);
    EXPECT_EQ(normalize_tray_popup_text_scale_percent(99), 100);
    EXPECT_EQ(normalize_tray_popup_text_scale_percent(226), 100);
    EXPECT_EQ(compute_tray_popup_font_height_px(13, 0), 13);
}

TEST(TrayMenuPopupModel, TextRowsExpandOnlyWhenFontNeedsMoreSpace) {
    EXPECT_EQ(compute_tray_popup_text_row_height(41, 13, 18), 41);
    EXPECT_EQ(compute_tray_popup_text_row_height(28, 29, 12), 41);
}

TEST(TrayMenuPopupModel, ChromeBoundsPreserveVisibleSurfaceAnchor) {
    const auto geometry = compute_tray_popup_chrome_geometry(
        776,
        508,
        420,
        483,
        18);

    EXPECT_EQ(geometry.window_x, 758);
    EXPECT_EQ(geometry.window_y, 490);
    EXPECT_EQ(geometry.window_width, 456);
    EXPECT_EQ(geometry.window_height, 519);
    EXPECT_EQ(geometry.surface_left, 18);
    EXPECT_EQ(geometry.surface_top, 18);
    EXPECT_EQ(geometry.window_x + geometry.surface_left, 776);
    EXPECT_EQ(geometry.window_y + geometry.surface_top, 508);
    EXPECT_EQ(geometry.surface_width, 420);
    EXPECT_EQ(geometry.surface_height, 483);
}

TEST(TrayMenuPopupModel, ChromeBoundsScaleInsetWithoutRescalingSurface) {
    const auto geometry = compute_tray_popup_chrome_geometry(
        100,
        200,
        560,
        644,
        32);

    EXPECT_EQ(geometry.window_x, 68);
    EXPECT_EQ(geometry.window_y, 168);
    EXPECT_EQ(geometry.window_width, 624);
    EXPECT_EQ(geometry.window_height, 708);
    EXPECT_EQ(geometry.surface_width, 560);
    EXPECT_EQ(geometry.surface_height, 644);
}

TEST(TrayMenuPopupModel, RoundedSurfaceHitTestRejectsShadowAndCornerPixels) {
    const auto geometry = compute_tray_popup_chrome_geometry(
        0,
        0,
        280,
        320,
        16);

    EXPECT_FALSE(tray_popup_point_in_rounded_surface(geometry, 0, 0, 12));
    EXPECT_FALSE(tray_popup_point_in_rounded_surface(geometry, 16, 16, 12));
    EXPECT_FALSE(tray_popup_point_in_rounded_surface(geometry, 17, 17, 12));
    EXPECT_TRUE(tray_popup_point_in_rounded_surface(geometry, 28, 16, 12));
    EXPECT_TRUE(tray_popup_point_in_rounded_surface(geometry, 156, 176, 12));
    EXPECT_TRUE(tray_popup_point_in_rounded_surface(geometry, 28, 335, 12));
    EXPECT_FALSE(tray_popup_point_in_rounded_surface(geometry, 296, 176, 12));
}

} // namespace
} // namespace acecode::desktop
