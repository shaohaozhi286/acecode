#pragma once

// Codex 风格 tray 菜单 layout 的纯函数版本。
// 把 TrayMenuPayload 转成一份"按顺序 Append 的 item list",让单测能不依赖 Win32
// 直接断言菜单结构(空状态、仅 pinned、超过 More 阈值等)。
//
// 真正的 AppendMenuW / TrackPopupMenu 调用在 tray_icon_win.cpp,根据这里产出的
// MenuLayout 一一映射。
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3 + 6。

#include "tray_icon_win.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::desktop {

// 菜单 ID 编码方案。1..49 留给固定项,100..199 pinned,200..299 recent 可见区,
// 300..399 More 子菜单中的 recent overflow。详见 design.md。
inline constexpr unsigned kMenuIdShow         = 1;  // 历史保留:行为等价 OpenApp
inline constexpr unsigned kMenuIdQuit         = 2;
inline constexpr unsigned kMenuIdNewChat      = 3;
inline constexpr unsigned kMenuIdOpenApp      = 4;
inline constexpr unsigned kMenuIdPinnedBase   = 100;
inline constexpr unsigned kMenuIdRecentBase   = 200;
inline constexpr unsigned kMenuIdMoreBase     = 300;

// 上限(决策 3):pinned 最多 5 个、recent 顶层可见 6 个、Recent 总数(含 More 子菜单)
// 不超过 14 个。subtitle 单行截断到 40 codepoint(简化:这里按 byte 截断,实现期
// 若发现 CJK 截断丑陋再切到 codepoint 安全版本)。
inline constexpr std::size_t kPinnedMax            = 5;
inline constexpr std::size_t kRecentVisible        = 6;
inline constexpr std::size_t kRecentMaxIncludingMore = 14;
inline constexpr std::size_t kSubtitleMaxBytes     = 40;

enum class TrayMenuEntryKind {
    PinnedHeader,    // disabled 灰色 "Pinned"
    PinnedItem,      // 一条 pinned session
    RecentHeader,    // disabled 灰色 "Recent"
    RecentItem,      // 一条 recent session(顶层可见)
    MoreSubmenuRoot, // "More ›" 入口(打开 More 子菜单)
    MoreSubmenuItem, // More 子菜单内一条 recent overflow(layout 中扁平列出,渲染时收进 popup)
    Separator,
    NewChat,
    OpenApp,
    Quit,
};

struct TrayMenuEntry {
    TrayMenuEntryKind kind;
    unsigned id = 0;        // 仅 *Item / NewChat / OpenApp / Quit / MoreSubmenuRoot 有意义
    std::string label;      // 渲染文本(显示用)— Header / Separator / MoreSubmenuRoot 走预设字串
    // 当 kind ∈ {PinnedItem, RecentItem, MoreSubmenuItem} 时,这两个字段引用 payload 里的 session
    std::string session_id;
    std::string workspace_hash;
};

struct TrayMenuLayout {
    std::vector<TrayMenuEntry> entries;
};

namespace detail {

// subtitle 截断:超过 kSubtitleMaxBytes 时切到 limit-3 byte 后追加 "..."。
// 用 ASCII "..." 而非 U+2026 "…" 让 byte boundary 与 codepoint 边界对齐(避开 UTF-8
// 截半字符的视觉异常);Win32 菜单字体里 "..." 视觉补救已足够。
inline std::string truncate_subtitle(std::string s, std::size_t limit_bytes = kSubtitleMaxBytes) {
    if (s.size() <= limit_bytes) return s;
    if (limit_bytes <= 3) return std::string("...").substr(0, limit_bytes);
    s.resize(limit_bytes - 3);
    s += "...";
    return s;
}

// 单条 session 渲染成菜单 label:"<title> · <subtitle>"(subtitle 空时只 title)。
inline std::string format_session_label(const TrayMenuItem& it) {
    std::string label = it.title;
    if (!it.subtitle.empty()) {
        label += "  ";
        label += truncate_subtitle(it.subtitle);
    }
    return label;
}

} // namespace detail

// 纯函数:把 payload 翻译为 layout。空状态(pinned + recent 都空)直接产出
// "新建会话 / 打开 / 退出" 极简菜单,没有空 header。
inline TrayMenuLayout compute_menu_layout(const TrayMenuPayload& payload) {
    TrayMenuLayout layout;
    auto& out = layout.entries;

    const std::size_t pinned_n = std::min(payload.pinned.size(), kPinnedMax);
    const std::size_t recent_total = std::min(payload.recent.size(), kRecentMaxIncludingMore);
    const std::size_t recent_visible = std::min(recent_total, kRecentVisible);
    const std::size_t recent_overflow =
        recent_total > recent_visible ? recent_total - recent_visible : 0;

    const bool has_any_session = (pinned_n + recent_total) > 0;

    if (pinned_n > 0) {
        out.push_back({TrayMenuEntryKind::PinnedHeader, 0, "Pinned", {}, {}});
        for (std::size_t i = 0; i < pinned_n; ++i) {
            const auto& it = payload.pinned[i];
            TrayMenuEntry e;
            e.kind = TrayMenuEntryKind::PinnedItem;
            e.id = static_cast<unsigned>(kMenuIdPinnedBase + i);
            e.label = detail::format_session_label(it);
            e.session_id = it.session_id;
            e.workspace_hash = it.workspace_hash;
            out.push_back(std::move(e));
        }
    }

    if (recent_total > 0) {
        if (pinned_n > 0) {
            out.push_back({TrayMenuEntryKind::Separator, 0, {}, {}, {}});
        }
        out.push_back({TrayMenuEntryKind::RecentHeader, 0, "Recent", {}, {}});
        for (std::size_t i = 0; i < recent_visible; ++i) {
            const auto& it = payload.recent[i];
            TrayMenuEntry e;
            e.kind = TrayMenuEntryKind::RecentItem;
            e.id = static_cast<unsigned>(kMenuIdRecentBase + i);
            e.label = detail::format_session_label(it);
            e.session_id = it.session_id;
            e.workspace_hash = it.workspace_hash;
            out.push_back(std::move(e));
        }
        if (recent_overflow > 0) {
            out.push_back({TrayMenuEntryKind::MoreSubmenuRoot, 0, "More", {}, {}});
            for (std::size_t i = 0; i < recent_overflow; ++i) {
                const auto& it = payload.recent[recent_visible + i];
                TrayMenuEntry e;
                e.kind = TrayMenuEntryKind::MoreSubmenuItem;
                e.id = static_cast<unsigned>(kMenuIdMoreBase + i);
                e.label = detail::format_session_label(it);
                e.session_id = it.session_id;
                e.workspace_hash = it.workspace_hash;
                out.push_back(std::move(e));
            }
        }
    }

    if (has_any_session) {
        out.push_back({TrayMenuEntryKind::Separator, 0, {}, {}, {}});
    }
    out.push_back({TrayMenuEntryKind::NewChat, kMenuIdNewChat, "新建会话", {}, {}});
    out.push_back({TrayMenuEntryKind::OpenApp, kMenuIdOpenApp, "打开 ACECode", {}, {}});
    out.push_back({TrayMenuEntryKind::Separator, 0, {}, {}, {}});
    out.push_back({TrayMenuEntryKind::Quit, kMenuIdQuit, "退出", {}, {}});

    return layout;
}

} // namespace acecode::desktop
