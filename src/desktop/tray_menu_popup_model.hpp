#pragma once

// Windows 自定义 tray popup 使用的纯数据模型。
//
// TrayMenuLayout 为了兼容原生 HMENU,把 More root 与它的 overflow session
// 连续扁平存放。自定义 popup 需要显式父子关系,这里负责把连续的
// MoreSubmenuItem 收进对应 MoreSubmenuRoot,让 Win32 渲染层只处理窗口、绘制
// 与输入事件。该文件不依赖 Win32,可直接由单元测试覆盖。

#include "tray_menu_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace acecode::desktop {

struct TrayPopupRow {
    TrayMenuEntry entry;
    std::vector<TrayMenuEntry> submenu_entries;
};

struct TrayPopupPosition {
    int x = 0;
    int y = 0;
};

inline TrayPopupPosition compute_tray_popup_position(
    int anchor_x,
    int anchor_y,
    int popup_width,
    int popup_height,
    int monitor_left,
    int monitor_top,
    int monitor_right,
    int monitor_bottom) {
    int x = anchor_x - popup_width;
    if (x < monitor_left && anchor_x + popup_width <= monitor_right) {
        x = anchor_x;
    }

    int y = anchor_y - popup_height;
    if (y < monitor_top && anchor_y + popup_height <= monitor_bottom) {
        y = anchor_y;
    }

    const int max_x = std::max(monitor_left, monitor_right - popup_width);
    const int max_y = std::max(monitor_top, monitor_bottom - popup_height);
    x = std::clamp(x, monitor_left, max_x);
    y = std::clamp(y, monitor_top, max_y);
    return {x, y};
}

inline bool tray_popup_entry_is_session(TrayMenuEntryKind kind) {
    return kind == TrayMenuEntryKind::PinnedItem ||
           kind == TrayMenuEntryKind::RecentItem ||
           kind == TrayMenuEntryKind::MoreSubmenuItem;
}

inline bool tray_popup_entry_is_header(TrayMenuEntryKind kind) {
    return kind == TrayMenuEntryKind::PinnedHeader ||
           kind == TrayMenuEntryKind::RecentHeader;
}

inline bool tray_popup_entry_is_fixed_action(TrayMenuEntryKind kind) {
    return kind == TrayMenuEntryKind::NewChat ||
           kind == TrayMenuEntryKind::OpenApp ||
           kind == TrayMenuEntryKind::Quit;
}

inline bool tray_popup_entry_is_selectable(TrayMenuEntryKind kind) {
    return tray_popup_entry_is_session(kind) ||
           kind == TrayMenuEntryKind::MoreSubmenuRoot ||
           tray_popup_entry_is_fixed_action(kind);
}

inline bool tray_popup_row_has_submenu(const TrayPopupRow& row) {
    return row.entry.kind == TrayMenuEntryKind::MoreSubmenuRoot &&
           !row.submenu_entries.empty();
}

inline std::vector<TrayPopupRow> build_tray_popup_rows(const TrayMenuLayout& layout) {
    std::vector<TrayPopupRow> rows;
    rows.reserve(layout.entries.size());

    for (std::size_t i = 0; i < layout.entries.size(); ++i) {
        const TrayMenuEntry& entry = layout.entries[i];
        if (entry.kind == TrayMenuEntryKind::MoreSubmenuItem) {
            // Orphan overflow entries are not valid top-level rows. Valid entries are
            // consumed by their preceding MoreSubmenuRoot below.
            continue;
        }

        TrayPopupRow row;
        row.entry = entry;
        if (entry.kind == TrayMenuEntryKind::MoreSubmenuRoot) {
            std::size_t next = i + 1;
            while (next < layout.entries.size() &&
                   layout.entries[next].kind == TrayMenuEntryKind::MoreSubmenuItem) {
                row.submenu_entries.push_back(layout.entries[next]);
                ++next;
            }
            i = next - 1;
        }
        // Codex-style action blocks are visually separated one-by-one. The
        // shared TrayMenuLayout keeps NewChat/OpenApp adjacent for native
        // backends, so insert only a popup-model divider between consecutive
        // fixed actions without changing their command order.
        if (tray_popup_entry_is_fixed_action(row.entry.kind) && !rows.empty() &&
            tray_popup_entry_is_fixed_action(rows.back().entry.kind)) {
            TrayMenuEntry separator;
            separator.kind = TrayMenuEntryKind::Separator;
            rows.push_back({std::move(separator), {}});
        }
        rows.push_back(std::move(row));
    }

    return rows;
}

inline std::vector<TrayPopupRow> build_tray_popup_submenu_rows(const TrayPopupRow& root) {
    std::vector<TrayPopupRow> rows;
    rows.reserve(root.submenu_entries.size());
    for (const auto& entry : root.submenu_entries) {
        rows.push_back({entry, {}});
    }
    return rows;
}

} // namespace acecode::desktop
