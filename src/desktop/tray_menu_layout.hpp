#pragma once

// Codex 风格 tray 菜单 layout 的纯函数版本。
// 把 TrayMenuPayload 转成一份"按顺序 Append 的 item list",让单测能不依赖 Win32
// 直接断言菜单结构(空状态、Pinned/Recent 各自 More 等)。
//
// 真正的 AppendMenuW / TrackPopupMenu 调用在 tray_icon_win.cpp,根据这里产出的
// MenuLayout 一一映射。
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3 + 6。

#include "tray_icon_win.hpp"
#include "strings.hpp"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace acecode::desktop {

// 菜单 ID 编码方案。1..49 留给固定项;session 项从 100 起按 layout 顺序分配,
// 点击派发时用 layout 里的 id -> session 元数据反查,避免 Pinned More 和
// Recent More 共用旧固定区间。
inline constexpr unsigned kMenuIdShow        = 1;  // 历史保留:行为等价 OpenApp
inline constexpr unsigned kMenuIdQuit        = 2;
inline constexpr unsigned kMenuIdNewChat     = 3;
inline constexpr unsigned kMenuIdOpenApp     = 4;
inline constexpr unsigned kMenuIdSessionBase = 100;

// 顶层每段最多显示 3 条;超过的条目进入本段自己的 More 子菜单。payload 不做
// 产品级总数截断,让 More 能显示全部剩余会话。
inline constexpr std::size_t kTraySectionVisibleLimit = 3;
inline constexpr std::size_t kSubtitleMaxBytes        = 40;

enum class TrayMenuEntryKind {
    PinnedHeader,    // disabled 灰色 "Pinned"
    PinnedItem,      // 一条 pinned session(顶层可见)
    RecentHeader,    // disabled 灰色 "Recent"
    RecentItem,      // 一条 recent session(顶层可见)
    MoreSubmenuRoot, // "More >" 入口(打开当前 section 的 More 子菜单)
    MoreSubmenuItem, // More 子菜单内一条 overflow session(layout 中扁平列出)
    Separator,
    NewChat,
    OpenApp,
    Quit,
};

struct TrayMenuEntry {
    TrayMenuEntryKind kind;
    unsigned id = 0;        // 仅 session item / NewChat / OpenApp / Quit 有意义
    std::string label;      // 普通菜单后端使用的渲染文本
    // session item 的两列显示文本。Win32 owner-draw 用 title/subtitle 分列绘制,
    // 其它后端继续用 label 退化为单行 "<title>  <subtitle>"。
    std::string title;
    std::string subtitle;
    // 当 kind 属于 session item 时,这两个字段引用 payload 里的 session。
    std::string session_id;
    std::string workspace_hash;
};

struct TrayMenuLayout {
    std::vector<TrayMenuEntry> entries;
};

namespace detail {

inline std::string session_key(const TrayMenuItem& it) {
    if (it.session_id.empty()) return {};
    return it.workspace_hash + '\0' + it.session_id;
}

inline std::vector<TrayMenuItem> unique_items(const std::vector<TrayMenuItem>& items,
                                              const std::unordered_set<std::string>& excluded = {}) {
    std::vector<TrayMenuItem> out;
    std::unordered_set<std::string> seen;
    for (const auto& it : items) {
        const std::string key = session_key(it);
        if (key.empty() || excluded.find(key) != excluded.end() || seen.find(key) != seen.end()) {
            continue;
        }
        seen.insert(key);
        out.push_back(it);
    }
    return out;
}

inline bool is_utf8_continuation_byte(unsigned char ch) {
    return (ch & 0xC0u) == 0x80u;
}

// subtitle 截断:超过 kSubtitleMaxBytes 时切到合法 UTF-8 边界后追加 "..."。
inline std::string truncate_subtitle(std::string s, std::size_t limit_bytes = kSubtitleMaxBytes) {
    if (s.size() <= limit_bytes) return s;
    if (limit_bytes <= 3) return std::string("...").substr(0, limit_bytes);
    std::size_t cut = limit_bytes - 3;
    while (cut > 0 && is_utf8_continuation_byte(static_cast<unsigned char>(s[cut]))) {
        --cut;
    }
    s.resize(cut);
    s += "...";
    return s;
}

// 单条 session 渲染成菜单 label:"<title>  <subtitle>"(subtitle 空时只 title)。
inline std::string format_session_label(const TrayMenuItem& it,
                                        const std::string& locale = "zh-CN") {
    std::string label = it.title.empty()
        ? std::string(desktop_string(DesktopStringId::NewSessionFallback, locale))
        : it.title;
    if (!it.subtitle.empty()) {
        label += "  ";
        label += truncate_subtitle(it.subtitle);
    }
    return label;
}

inline void append_session_item(std::vector<TrayMenuEntry>& out,
                                TrayMenuEntryKind kind,
                                const TrayMenuItem& it,
                                unsigned& next_session_id,
                                const std::string& locale) {
    const std::string title = it.title.empty()
        ? std::string(desktop_string(DesktopStringId::NewSessionFallback, locale))
        : it.title;
    const std::string subtitle = it.subtitle.empty() ? std::string{} : truncate_subtitle(it.subtitle);
    TrayMenuEntry e;
    e.kind = kind;
    e.id = next_session_id++;
    e.label = title;
    if (!subtitle.empty()) {
        e.label += "  ";
        e.label += subtitle;
    }
    e.title = title;
    e.subtitle = subtitle;
    e.session_id = it.session_id;
    e.workspace_hash = it.workspace_hash;
    out.push_back(std::move(e));
}

inline void append_section(std::vector<TrayMenuEntry>& out,
                           TrayMenuEntryKind header_kind,
                           TrayMenuEntryKind visible_kind,
                           const std::string& header_label,
                           const std::string& more_label,
                           const std::vector<TrayMenuItem>& items,
                           unsigned& next_session_id,
                           const std::string& locale) {
    if (items.empty()) return;
    out.push_back({header_kind, 0, header_label, {}, {}});

    const std::size_t visible = items.size() < kTraySectionVisibleLimit
        ? items.size()
        : kTraySectionVisibleLimit;
    for (std::size_t i = 0; i < visible; ++i) {
        append_session_item(out, visible_kind, items[i], next_session_id, locale);
    }

    if (items.size() > visible) {
        out.push_back({TrayMenuEntryKind::MoreSubmenuRoot, 0, more_label, {}, {}});
        for (std::size_t i = visible; i < items.size(); ++i) {
            append_session_item(out, TrayMenuEntryKind::MoreSubmenuItem,
                                items[i], next_session_id, locale);
        }
    }
}

} // namespace detail

// 纯函数:把 payload 翻译为 layout。空状态(pinned + recent 都空)直接产出
// "新建会话 / 打开 / 退出" 极简菜单,没有空 header。
inline TrayMenuLayout compute_menu_layout(const TrayMenuPayload& payload,
                                          const std::string& locale = "zh-CN") {
    TrayMenuLayout layout;
    auto& out = layout.entries;
    unsigned next_session_id = kMenuIdSessionBase;

    const std::vector<TrayMenuItem> pinned = detail::unique_items(payload.pinned);
    std::unordered_set<std::string> pinned_keys;
    for (const auto& it : pinned) pinned_keys.insert(detail::session_key(it));
    const std::vector<TrayMenuItem> recent = detail::unique_items(payload.recent, pinned_keys);

    if (!pinned.empty()) {
        detail::append_section(out,
                               TrayMenuEntryKind::PinnedHeader,
                               TrayMenuEntryKind::PinnedItem,
                               std::string(desktop_string(DesktopStringId::TrayPinned, locale)),
                               std::string(desktop_string(DesktopStringId::TrayMore, locale)),
                               pinned,
                               next_session_id,
                               locale);
    }

    if (!recent.empty()) {
        if (!pinned.empty()) {
            out.push_back({TrayMenuEntryKind::Separator, 0, {}, {}, {}});
        }
        detail::append_section(out,
                               TrayMenuEntryKind::RecentHeader,
                               TrayMenuEntryKind::RecentItem,
                               std::string(desktop_string(DesktopStringId::TrayRecent, locale)),
                               std::string(desktop_string(DesktopStringId::TrayMore, locale)),
                               recent,
                               next_session_id,
                               locale);
    }

    if (!pinned.empty() || !recent.empty()) {
        out.push_back({TrayMenuEntryKind::Separator, 0, {}, {}, {}});
    }
    out.push_back({TrayMenuEntryKind::NewChat, kMenuIdNewChat,
                   std::string(desktop_string(DesktopStringId::TrayNewSession, locale)), {}, {}});
    out.push_back({TrayMenuEntryKind::OpenApp, kMenuIdOpenApp,
                   std::string(desktop_string(DesktopStringId::TrayOpenApp, locale)), {}, {}});
    out.push_back({TrayMenuEntryKind::Separator, 0, {}, {}, {}});
    out.push_back({TrayMenuEntryKind::Quit, kMenuIdQuit,
                   std::string(desktop_string(DesktopStringId::TrayQuit, locale)), {}, {}});

    return layout;
}

} // namespace acecode::desktop
