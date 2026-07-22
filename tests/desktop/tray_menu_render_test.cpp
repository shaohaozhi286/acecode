// tray_menu_layout.hpp 的 compute_menu_layout 纯函数测试。
//
// 验收点(按 spec.md 要求):
//   - 空 payload → "新建会话 / 打开 ACECode / 退出",无空 Pinned/Recent header
//   - Pinned / Recent 顶层各最多 3 条
//   - Pinned More 和 Recent More 各自收本段剩余会话
//   - Recent 不重复 pinned
//   - More 不包含顶层已展示 session,也不再被旧 14 条上限截断
//   - session item id 按 layout 顺序唯一分配,用于点击派发反查
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3 / 6。

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "desktop/tray_menu_layout.hpp"

using namespace acecode::desktop;

namespace {

TrayMenuPayload empty_payload() { return TrayMenuPayload{}; }

TrayMenuItem mk(const std::string& sid,
                const std::string& title,
                const std::string& subtitle = "",
                const std::string& workspace_hash = "ws") {
    TrayMenuItem it;
    it.session_id = sid;
    it.workspace_hash = workspace_hash;
    it.title = title;
    it.subtitle = subtitle;
    return it;
}

std::size_t count_kind(const TrayMenuLayout& l, TrayMenuEntryKind k) {
    std::size_t n = 0;
    for (const auto& e : l.entries) if (e.kind == k) ++n;
    return n;
}

std::vector<std::string> session_ids_for_kind(const TrayMenuLayout& l, TrayMenuEntryKind k) {
    std::vector<std::string> out;
    for (const auto& e : l.entries) {
        if (e.kind == k) out.push_back(e.session_id);
    }
    return out;
}

std::vector<unsigned> session_command_ids(const TrayMenuLayout& l) {
    std::vector<unsigned> out;
    for (const auto& e : l.entries) {
        if (e.kind == TrayMenuEntryKind::PinnedItem ||
            e.kind == TrayMenuEntryKind::RecentItem ||
            e.kind == TrayMenuEntryKind::MoreSubmenuItem) {
            out.push_back(e.id);
        }
    }
    return out;
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
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::Separator), 1u);
}

TEST(TrayMenuLayout, FixedChromeLocalizesAndSessionTitlesStayOpaque) {
    TrayMenuPayload payload;
    payload.pinned.push_back(mk("p1", u8"用户标题", "workspace"));
    payload.recent.push_back(mk("r1", "", "workspace"));
    const auto layout = compute_menu_layout(payload, "en-US");

    EXPECT_EQ(layout.entries[0].label, "Pinned");
    bool kept_user_title = false;
    bool localized_fallback = false;
    bool localized_new_action = false;
    for (const auto& entry : layout.entries) {
        if (entry.session_id == "p1") {
            EXPECT_EQ(entry.title, u8"用户标题");
            kept_user_title = true;
        }
        if (entry.session_id == "r1") {
            EXPECT_EQ(entry.title, "New session 1");
            localized_fallback = true;
        }
        if (entry.kind == TrayMenuEntryKind::NewChat) {
            EXPECT_EQ(entry.label, "New session");
            localized_new_action = true;
        }
    }
    EXPECT_TRUE(kept_user_title);
    EXPECT_TRUE(localized_fallback);
    EXPECT_TRUE(localized_new_action);
}

// 场景:pinned 正好 3 条 — 顶层全展示,不开 More
TEST(TrayMenuLayout, PinnedExactlyThreeNoMoreSubmenu) {
    TrayMenuPayload p;
    for (int i = 0; i < 3; ++i) p.pinned.push_back(mk("p" + std::to_string(i), "P"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedHeader), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedItem), 3u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 0u);
}

// 场景:pinned 超过 3 条 — 前 3 条顶层展示,剩余进 Pinned More
TEST(TrayMenuLayout, PinnedOverflowsIntoOwnMoreSubmenu) {
    TrayMenuPayload p;
    for (int i = 0; i < 5; ++i) p.pinned.push_back(mk("p" + std::to_string(i), "P"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::PinnedItem), 3u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 2u);
    EXPECT_EQ(session_ids_for_kind(layout, TrayMenuEntryKind::PinnedItem),
              (std::vector<std::string>{"p0", "p1", "p2"}));
    EXPECT_EQ(session_ids_for_kind(layout, TrayMenuEntryKind::MoreSubmenuItem),
              (std::vector<std::string>{"p3", "p4"}));
}

// 场景:recent 正好 3 条 — 顶层全展示,不开 More
TEST(TrayMenuLayout, RecentExactlyThreeNoMoreSubmenu) {
    TrayMenuPayload p;
    for (int i = 0; i < 3; ++i) p.recent.push_back(mk("r" + std::to_string(i), "R"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentHeader), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 3u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 0u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 0u);
}

// 场景:recent 超过 3 条 — 前 3 条顶层展示,剩余进 Recent More
TEST(TrayMenuLayout, RecentOverflowsIntoOwnMoreSubmenu) {
    TrayMenuPayload p;
    for (int i = 0; i < 6; ++i) p.recent.push_back(mk("r" + std::to_string(i), "R"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 3u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 1u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 3u);
    EXPECT_EQ(session_ids_for_kind(layout, TrayMenuEntryKind::RecentItem),
              (std::vector<std::string>{"r0", "r1", "r2"}));
    EXPECT_EQ(session_ids_for_kind(layout, TrayMenuEntryKind::MoreSubmenuItem),
              (std::vector<std::string>{"r3", "r4", "r5"}));
}

// 场景:pinned + recent 都超过 3 条 — 两个 section 各自拥有一个 More
TEST(TrayMenuLayout, MixedPayloadHasTwoIndependentMoreSubmenus) {
    TrayMenuPayload p;
    for (int i = 0; i < 5; ++i) p.pinned.push_back(mk("p" + std::to_string(i), "P"));
    for (int i = 0; i < 5; ++i) p.recent.push_back(mk("r" + std::to_string(i), "R"));
    auto layout = compute_menu_layout(p);

    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuRoot), 2u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 4u);

    ASSERT_GE(layout.entries.size(), 19u);
    EXPECT_EQ(layout.entries[0].kind, TrayMenuEntryKind::PinnedHeader);
    EXPECT_EQ(layout.entries[1].kind, TrayMenuEntryKind::PinnedItem);
    EXPECT_EQ(layout.entries[2].kind, TrayMenuEntryKind::PinnedItem);
    EXPECT_EQ(layout.entries[3].kind, TrayMenuEntryKind::PinnedItem);
    EXPECT_EQ(layout.entries[4].kind, TrayMenuEntryKind::MoreSubmenuRoot);
    EXPECT_EQ(layout.entries[5].kind, TrayMenuEntryKind::MoreSubmenuItem);
    EXPECT_EQ(layout.entries[6].kind, TrayMenuEntryKind::MoreSubmenuItem);
    EXPECT_EQ(layout.entries[7].kind, TrayMenuEntryKind::Separator);
    EXPECT_EQ(layout.entries[8].kind, TrayMenuEntryKind::RecentHeader);
    EXPECT_EQ(layout.entries[9].kind, TrayMenuEntryKind::RecentItem);
    EXPECT_EQ(layout.entries[10].kind, TrayMenuEntryKind::RecentItem);
    EXPECT_EQ(layout.entries[11].kind, TrayMenuEntryKind::RecentItem);
    EXPECT_EQ(layout.entries[12].kind, TrayMenuEntryKind::MoreSubmenuRoot);
    EXPECT_EQ(layout.entries[13].kind, TrayMenuEntryKind::MoreSubmenuItem);
    EXPECT_EQ(layout.entries[14].kind, TrayMenuEntryKind::MoreSubmenuItem);
    EXPECT_EQ(layout.entries[15].kind, TrayMenuEntryKind::Separator);
    EXPECT_EQ(layout.entries[16].kind, TrayMenuEntryKind::NewChat);
    EXPECT_EQ(layout.entries[17].kind, TrayMenuEntryKind::OpenApp);
    EXPECT_EQ(layout.entries[18].kind, TrayMenuEntryKind::Separator);
}

// 场景:recent 输入重复了 pinned — pinned 优先,recent 不再展示同一 session
TEST(TrayMenuLayout, RecentDoesNotDuplicatePinnedSessions) {
    TrayMenuPayload p;
    p.pinned = { mk("same", "Pinned", "", "ws-a") };
    p.recent = {
        mk("same", "Recent duplicate", "", "ws-a"),
        mk("other", "Recent other", "", "ws-a"),
    };
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(session_ids_for_kind(layout, TrayMenuEntryKind::PinnedItem),
              (std::vector<std::string>{"same"}));
    EXPECT_EQ(session_ids_for_kind(layout, TrayMenuEntryKind::RecentItem),
              (std::vector<std::string>{"other"}));
}

// 场景:超长 More 不再按旧 14 条总上限截断
TEST(TrayMenuLayout, MoreSubmenuKeepsAllOverflowItems) {
    TrayMenuPayload p;
    for (int i = 0; i < 30; ++i) p.recent.push_back(mk("r" + std::to_string(i), "R"));
    auto layout = compute_menu_layout(p);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::RecentItem), 3u);
    EXPECT_EQ(count_kind(layout, TrayMenuEntryKind::MoreSubmenuItem), 27u);
}

// 场景:session command id 唯一,并按 layout 顺序从 kMenuIdSessionBase 开始
TEST(TrayMenuLayout, SessionCommandIdsAreUniqueAndSequential) {
    TrayMenuPayload p;
    for (int i = 0; i < 4; ++i) p.pinned.push_back(mk("p" + std::to_string(i), "P"));
    for (int i = 0; i < 4; ++i) p.recent.push_back(mk("r" + std::to_string(i), "R"));
    auto ids = session_command_ids(compute_menu_layout(p));

    ASSERT_EQ(ids.size(), 8u);
    std::set<unsigned> unique(ids.begin(), ids.end());
    EXPECT_EQ(unique.size(), ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(ids[i], kMenuIdSessionBase + static_cast<unsigned>(i));
    }
}

// 场景:subtitle 截断 — 超过 40 字节尾部追 "..."
TEST(TrayMenuLayout, SubtitleTruncatedWithEllipsis) {
    std::string long_subtitle(80, 'A');
    TrayMenuPayload p;
    p.recent.push_back(mk("r0", "T", long_subtitle));
    auto layout = compute_menu_layout(p);
    for (const auto& e : layout.entries) {
        if (e.kind == TrayMenuEntryKind::RecentItem) {
            EXPECT_LE(e.label.size(), 1u + 2u + 40u);
            ASSERT_GE(e.label.size(), 3u);
            EXPECT_EQ(e.label.substr(e.label.size() - 3), "...");
            EXPECT_EQ(e.title, "T");
            EXPECT_LE(e.subtitle.size(), 40u);
            EXPECT_EQ(e.subtitle.substr(e.subtitle.size() - 3), "...");
        }
    }
}

// 场景:layout 保留 title/subtitle 分列字段,供 Win32 owner-draw 右对齐项目名。
TEST(TrayMenuLayout, SessionItemsKeepSeparateTitleAndSubtitle) {
    TrayMenuPayload p;
    p.recent.push_back(mk("r0", "检查上下文压缩缺失", "acecode"));
    auto layout = compute_menu_layout(p);

    bool found = false;
    for (const auto& e : layout.entries) {
        if (e.kind == TrayMenuEntryKind::RecentItem) {
            EXPECT_EQ(e.title, "检查上下文压缩缺失");
            EXPECT_EQ(e.subtitle, "acecode");
            EXPECT_EQ(e.label, "检查上下文压缩缺失  acecode");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// 场景:Item 携带 session_id + workspace_hash 让 click 派发能反查
TEST(TrayMenuLayout, ItemsCarrySessionMetadata) {
    TrayMenuPayload p;
    p.recent.push_back(mk("session-aaa", "Hi", "", "ws-zzz"));
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
