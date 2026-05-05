// notifications_win.{hpp,cpp} 公共契约的纯逻辑测试。
//
// 该模块大部分 API 只在 Windows 上有实质行为(Shell_NotifyIcon 调用),非 Windows
// 平台是 no-op 桩(见 openspec/changes/add-desktop-attention-notifications)。
// 这里只测可移植契约:
//   - NotifyPayload 默认 ctor 字段为空字符串
//   - init_notifications(nullptr) 返回 false 不崩
//   - 未 init 时 show_notification / on_balloon_clicked / shutdown 都 no-op 不崩
//   - set_click_handler 接受 nullptr handler 不崩
//
// 真正的端到端 toast 交互需要真机 + 用户确认,放到 tasks.md 的手测清单里。

#include <gtest/gtest.h>

#include "desktop/notifications_win.hpp"

using namespace acecode::desktop;

TEST(DesktopNotificationsPayload, DefaultsAreEmpty) {
    NotifyPayload p;
    EXPECT_EQ(p.id, "");
    EXPECT_EQ(p.workspace_hash, "");
    EXPECT_EQ(p.session_id, "");
    EXPECT_EQ(p.title, "");
    EXPECT_EQ(p.body, "");
}

TEST(DesktopNotificationsPayload, AssignAllFieldsCopiesCleanly) {
    NotifyPayload p;
    p.id = "n-1";
    p.workspace_hash = "ws-abc";
    p.session_id = "s-xyz";
    p.title = "需要你回答 · 测试会话";
    p.body  = "请确认是否继续";
    NotifyPayload copy = p;
    EXPECT_EQ(copy.id, "n-1");
    EXPECT_EQ(copy.workspace_hash, "ws-abc");
    EXPECT_EQ(copy.session_id, "s-xyz");
    EXPECT_EQ(copy.title, "需要你回答 · 测试会话");
    EXPECT_EQ(copy.body, "请确认是否继续");
}

TEST(DesktopNotificationsLifecycle, InitWithNullHwndReturnsFalseAndIsSafe) {
    // 不传 tray HWND → 视为 init 失败,后续 show_notification 应 no-op。
    EXPECT_FALSE(init_notifications(nullptr));
    NotifyPayload p;
    p.title = "x";
    p.body = "y";
    EXPECT_NO_THROW(show_notification(p));
    EXPECT_NO_THROW(on_balloon_clicked());
    EXPECT_NO_THROW(shutdown_notifications());
}

TEST(DesktopNotificationsLifecycle, RepeatedShutdownIsSafe) {
    EXPECT_NO_THROW(shutdown_notifications());
    EXPECT_NO_THROW(shutdown_notifications());
    EXPECT_NO_THROW(shutdown_notifications());
}

TEST(DesktopNotificationsLifecycle, SetClickHandlerNullptrIsSafe) {
    EXPECT_NO_THROW(set_click_handler(nullptr));
    EXPECT_NO_THROW(on_balloon_clicked());
}

TEST(DesktopNotificationsLifecycle, SetClickHandlerAcceptsLambda) {
    int seen = 0;
    set_click_handler([&seen](const std::string&, const std::string&, const std::string&) {
        ++seen;
    });
    // 未 init 状态 → 不应触发 handler
    on_balloon_clicked();
    EXPECT_EQ(seen, 0);
    // 清理避免后续测试残留
    set_click_handler(nullptr);
}
