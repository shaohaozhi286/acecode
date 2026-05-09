// tray_menu_layout.hpp 的 compute_menu_layout 纯函数测试。
//
// 验收点(按 spec.md 要求):
//   - 空 payload → "新建会话 / 打开 ACECode / 退出",无空 Pinned/Recent header
//   - 仅 pinned → Pinned header + items + (无 Recent 段) + actions
//   - 仅 recent ≤ 6 → 不渲染 More 子菜单
//   - recent 超过 6 → 顶层 6 条 + More 子菜单收 overflow,overflow 上限 14-6=8
//   - pinned + recent 都满 → 上限分别 5 / 14(总数 14 含 More 内的)
//   - subtitle 截断到 40 字节内,> 40 时尾部含 "..."
//   - ID 编码:pinned 100..104,recent 顶层 200..205,More 内 300..307
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3 / 6。

#include <gtest/gtest.h>

#include "desktop/tray_menu_layout.hpp"

using namespace acecode::desktop;

namespace {

TrayMenuPayload empty_payload() { return TrayMenuPayload{}; }

TrayMenuItem mk(const std::string& sid, const std::string& title, const std::string& subtitle = "") {
    TrayMenuItem it;
    it.session_id = sid;
    it.workspace_hash = "ws";
    it.title = title;
    it.subtitle = subtitle;
    return it;
}

std::size_t count_kind(const TrayMenuLayout& l, TrayMenuEntryKind k) {
    std::size_t n = 0;
    for (const auto& e : l.entries) if (e.kind == k) ++n;
    return n;
}

bool has_id(const TrayMenuLayout& l, unsigned id) {
    for (const auto& e : l.entries) if (e.id == id) return true;
    return false;
}

} // namespace

// 场景:空 payload — 仅显示 "新建会话 / 打开 ACECode / 退出",无 header / 无 separator-before-actions
TEST(TrayMenuLayout, EmptyPayloadCollapsesToActions) {
    auto layout = compute_menu_layout(empty_payload());
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedHeader), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentHeader), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedItem), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::NewChat), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::OpenApp), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::Quit), 1u);
    // 仅 actions 之间的一道分隔(NewChat/OpenApp 与 Quit 之间);actions 之前不再有 separator
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::Separator), 1u);
}

// 场景:仅 pinned — 无 Recent header,actions 前有 separator
TEST(TrayMenuLayout, PinnedOnly) {
    TrayMenuPayload p;
    p.pinned = { mk("p1", "Hello"), mk("p2", "World") };
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedHeader), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedItem), 2u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentHeader), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 0u);
    // separator 数:pinned/actions 之间 1 + actions 内部 1 = 2
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::Separator), 2u);
    EXPECT_TRUE(has_id(layout, kMenuIdPinnedBase + 0));
    EXPECT_TRUE(has_id(layout, kMenuIdPinnedBase + 1));
}

// 场景:仅 recent ≤ 6 — 不开 More 子菜单
TEST(TrayMenuLayout, RecentBelowOverflow) {
    TrayMenuPayload p;
    for (int i = 0; i < 4; ++i) p.recent.push_back(mk("r" + std::to_string(i), "title"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 4u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 0u);
}

// 场景:recent 正好 6 — 仍不开 More(只在 > 6 才开)
TEST(TrayMenuLayout, RecentExactlySixNoMoreSubmenu) {
    TrayMenuPayload p;
    for (int i = 0; i < 6; ++i) p.recent.push_back(mk("r" + std::to_string(i), "title"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 6u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 0u);
}

// 场景:recent 超过 6 — 顶层 6 条 + More 子菜单收 overflow
TEST(TrayMenuLayout, RecentOverflowsIntoMoreSubmenu) {
    TrayMenuPayload p;
    for (int i = 0; i < 10; ++i) p.recent.push_back(mk("r" + std::to_string(i), "title"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 6u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 4u);
    // ID:顶层 200..205,More 子菜单 300..303
    for (unsigned i = 0; i < 6; ++i) EXPECT_TRUE(has_id(layout, kMenuIdRecentBase + i));
    for (unsigned i = 0; i < 4; ++i) EXPECT_TRUE(has_id(layout, kMenuIdMoreBase + i));
}

// 场景:超 14 条 recent → 14 即上限,More 子菜单最多 14-6 = 8 条
TEST(TrayMenuLayout, RecentCappedAtFourteenIncludingMore) {
    TrayMenuPayload p;
    for (int i = 0; i < 30; ++i) p.recent.push_back(mk("r" + std::to_string(i), "title"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 6u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 8u);
}

// 场景:pinned 超 5 上限 → 截到 5
TEST(TrayMenuLayout, PinnedCappedAtFive) {
    TrayMenuPayload p;
    for (int i = 0; i < 12; ++i) p.pinned.push_back(mk("p" + std::to_string(i), "x"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedItem), 5u);
}

// 场景:同时存在 pinned + recent — 顺序 / separator 都对
TEST(TrayMenuLayout, MixedPayloadOrdering) {
    TrayMenuPayload p;
    for (int i = 0; i < 2; ++i) p.pinned.push_back(mk("p" + std::to_string(i), "P"));
    for (int i = 0; i < 3; ++i) p.recent.push_back(mk("r" + std::to_string(i), "R"));
    auto layout = compute_menu_layout(p);

    // 期望顺序:PinnedHeader / 2x PinnedItem / Sep / RecentHeader / 3x RecentItem / Sep / NewChat / OpenApp / Sep / Quit
    ASSERT_GE(layout.entries.size(), 11u);
    EXPECT_EQ(layout.entries[0].kind, TrayMenuEntryKind::PinnedHeader);
    EXPECT_EQ(layout.entries[1].kind, TrayMenuEntryKind::PinnedItem);
    EXPECT_EQ(layout.entries[2].kind, TrayMenuEntryKind::PinnedItem);
    EXPECT_EQ(layout.entries[3].kind, TrayMenuEntryKind::Separator);
    EXPECT_EQ(layout.entries[4].kind, TrayMenuEntryKind::RecentHeader);
    EXPECT_EQ(layout.entries[5].kind, TrayMenuEntryKind::RecentItem);
    EXPECT_EQ(layout.entries[6].kind, TrayMenuEntryKind::RecentItem);
    EXPECT_EQ(layout.entries[7].kind, TrayMenuEntryKind::RecentItem);
    EXPECT_EQ(layout.entries[8].kind, TrayMenuEntryKind::Separator);
    EXPECT_EQ(layout.entries[9].kind, TrayMenuEntryKind::NewChat);
    EXPECT_EQ(layout.entries[10].kind, TrayMenuEntryKind::OpenApp);
    EXPECT_EQ(layout.entries[11].kind, TrayMenuEntryKind::Separator);
    EXPECT_EQ(layout.entries[12].kind, TrayMenuEntryKind::Quit);
}

// 场景:subtitle 截断 — 超过 40 字节尾部追 "..."
TEST(TrayMenuLayout, SubtitleTruncatedWithEllipsis) {
    std::string long_subtitle(80, 'A');
    TrayMenuPayload p;
    p.recent.push_back(mk("r0", "T", long_subtitle));
    auto layout = compute_menu_layout(p);
    // 找到对应 RecentItem 的 label
    for (const auto& e : layout.entries) {
        if (e.kind == TrayMenuEntryKind::RecentItem) {
            // label = "T  <truncated>" → 总长度 = 1 + 2 + 40 = 43
            EXPECT_LE(e.label.size(), 1u + 2u + 40u);
            // 截断版必须以 "..." 结尾
            ASSERT_GE(e.label.size(), 3u);
            EXPECT_EQ(e.label.substr(e.label.size() - 3), "...");
        }
    }
}

// 场景:subtitle 不足 40 字节 — 不被截断,无 "..."
TEST(TrayMenuLayout, SubtitleShortNotTruncated) {
    TrayMenuPayload p;
    p.recent.push_back(mk("r0", "T", "短副标题"));
    auto layout = compute_menu_layout(p);
    for (const auto& e : layout.entries) {
        if (e.kind == TrayMenuEntryKind::RecentItem) {
            EXPECT_EQ(e.label, "T  短副标题");
        }
    }
}

// 场景:Item 携带 session_id + workspace_hash 让 click 派发能反查
TEST(TrayMenuLayout, ItemsCarrySessionMetadata) {
    TrayMenuPayload p;
    p.recent.push_back(mk("session-aaa", "Hi"));
    p.recent.back().workspace_hash = "ws-zzz";
    auto layout = compute_menu_layout(p);
    bool found = false;
    for (const auto& e : layout.entries) {
        if (e.kind == TrayMenuEntryKind::RecentItem) {
            EXPECT_EQ(e.session_id, "session-aaa");
            EXPECT_EQ(e.workspace_hash, "ws-zzz");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}
