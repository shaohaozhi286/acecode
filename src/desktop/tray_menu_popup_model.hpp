#pragma once

// Windows 自定义 tray popup 使用的纯数据模型。
//
// TrayMenuLayout 为了兼容原生 HMENU,把 More root 与它的 overflow session
// 连续扁平存放。自定义 popup 需要显式父子关系,这里负责把连续的
// MoreSubmenuItem 收进对应 MoreSubmenuRoot,让 Win32 渲染层只处理窗口、绘制
// 与输入事件。该文件不依赖 Win32,可直接由单元测试覆盖。

#include "tray_menu_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
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

struct TrayPopupChromeGeometry {
    int window_x = 0;
    int window_y = 0;
    int window_width = 0;
    int window_height = 0;
    int surface_left = 0;
    int surface_top = 0;
    int surface_width = 0;
    int surface_height = 0;
};

inline int compute_tray_popup_geometry_dpi_from_monitor_scale_percent(
    int monitor_scale_percent) {
    if (monitor_scale_percent < 100 || monitor_scale_percent > 500) {
        return 96;
    }
    return static_cast<int>(
        (96LL * monitor_scale_percent + 50) / 100);
}

inline int scale_tray_popup_size_px(int size_dip, int geometry_dpi) {
    const int size = std::max(0, size_dip);
    const int dpi = geometry_dpi > 0 ? geometry_dpi : 96;
    return static_cast<int>(
        (static_cast<long long>(size) * dpi + 48) / 96);
}

inline int normalize_tray_popup_text_scale_percent(int text_scale_percent) {
    if (text_scale_percent < 100 || text_scale_percent > 225) {
        return 100;
    }
    return text_scale_percent;
}

inline int compute_tray_popup_font_height_px(
    int base_font_height_dip,
    int geometry_dpi,
    int text_scale_percent) {
    const int base_height = std::max(1, base_font_height_dip);
    const int dpi = geometry_dpi > 0 ? geometry_dpi : 96;
    const int text_scale =
        normalize_tray_popup_text_scale_percent(text_scale_percent);
    constexpr long long kScaleDenominator = 96LL * 100;
    return std::max(
        1,
        static_cast<int>(
            (static_cast<long long>(base_height) * dpi * text_scale +
             kScaleDenominator / 2) /
            kScaleDenominator));
}

inline int compute_tray_popup_text_row_height(
    int geometry_height_px,
    int font_height_px,
    int vertical_padding_px) {
    return std::max(
        std::max(1, geometry_height_px),
        std::max(1, font_height_px) + std::max(0, vertical_padding_px));
}

inline TrayPopupChromeGeometry compute_tray_popup_chrome_geometry(
    int surface_x,
    int surface_y,
    int surface_width,
    int surface_height,
    int chrome_inset) {
    const int inset = std::max(0, chrome_inset);
    const int width = std::max(0, surface_width);
    const int height = std::max(0, surface_height);
    return {
        surface_x - inset,
        surface_y - inset,
        width + inset * 2,
        height + inset * 2,
        inset,
        inset,
        width,
        height,
    };
}

inline double tray_popup_rounded_rect_distance(
    double x,
    double y,
    int width,
    int height,
    int radius) {
    if (width <= 0 || height <= 0) {
        return std::numeric_limits<double>::infinity();
    }
    const double half_width = static_cast<double>(width) / 2.0;
    const double half_height = static_cast<double>(height) / 2.0;
    const double clamped_radius = std::clamp(
        static_cast<double>(std::max(0, radius)),
        0.0,
        std::min(half_width, half_height));
    const double qx = std::abs(x - half_width) -
        (half_width - clamped_radius);
    const double qy = std::abs(y - half_height) -
        (half_height - clamped_radius);
    const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    const double inside = std::min(std::max(qx, qy), 0.0);
    return outside + inside - clamped_radius;
}

inline bool tray_popup_point_in_rounded_surface(
    const TrayPopupChromeGeometry& geometry,
    int window_x,
    int window_y,
    int corner_radius) {
    const int local_x = window_x - geometry.surface_left;
    const int local_y = window_y - geometry.surface_top;
    if (local_x < 0 || local_y < 0 ||
        local_x >= geometry.surface_width ||
        local_y >= geometry.surface_height) {
        return false;
    }
    return tray_popup_rounded_rect_distance(
               static_cast<double>(local_x) + 0.5,
               static_cast<double>(local_y) + 0.5,
               geometry.surface_width,
               geometry.surface_height,
               corner_radius) <= 0.0;
}

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
