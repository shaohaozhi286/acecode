// web_host_close_policy.hpp 的纯函数测试 — 验证 WebHost 关窗派发策略不变量。
//
// 真实 WndProc 在 web_host.cpp 的 kRequestQuitMsg / WM_CLOSE 分支里调用这两个
// 派发函数,但 web_host.cpp 因为 webview 依赖被排除在 acecode_testable 外,
// 所以策略函数提取到独立 hpp,这里只测策略本身。
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 2。
//
// 验收点:
//   - close handler 返回 true → ConsumedByHandler(隐藏到托盘路径)
//   - close handler 返回 false → FallthroughToDestroy(走 DestroyWindow)
//   - close handler 未注册(空 std::function)→ FallthroughToDestroy
//   - request_quit 永远走 FallthroughToDestroy(绕过 close handler 的不变量)
//   - close handler 真的被调用了一次(用 counter 验证)

#include <gtest/gtest.h>

#include "desktop/web_host_close_policy.hpp"

#include <functional>

using namespace acecode::desktop;

// 场景:close handler 返回 true 被消化,不走 DestroyWindow
TEST(WebHostCloseDispatch, HandlerConsumesCloseRequest) {
    int call_count = 0;
    std::function<bool()> handler = [&] {
        ++call_count;
        return true;
    };
    EXPECT_EQ(dispatch_wm_close(handler), CloseDispatch::ConsumedByHandler);
    EXPECT_EQ(call_count, 1);
}

// 场景:close handler 返回 false → 仍 fallthrough 到 DestroyWindow
TEST(WebHostCloseDispatch, HandlerReturnsFalseFallsThrough) {
    int call_count = 0;
    std::function<bool()> handler = [&] {
        ++call_count;
        return false;
    };
    EXPECT_EQ(dispatch_wm_close(handler), CloseDispatch::FallthroughToDestroy);
    EXPECT_EQ(call_count, 1);
}

// 场景:close handler 未注册 → fallthrough(原始 destroy 行为)
TEST(WebHostCloseDispatch, NoHandlerFallsThrough) {
    std::function<bool()> empty;
    EXPECT_EQ(dispatch_wm_close(empty), CloseDispatch::FallthroughToDestroy);
}

// 场景:request_quit 不走 handler,固定 destroy
TEST(WebHostCloseDispatch, RequestQuitAlwaysDestroys) {
    EXPECT_EQ(dispatch_request_quit(), CloseDispatch::FallthroughToDestroy);
}

// 场景:同一 handler 多次 wm_close 各被调一次(确认没有奇怪缓存)
TEST(WebHostCloseDispatch, HandlerInvokedEachCall) {
    int call_count = 0;
    std::function<bool()> handler = [&] {
        ++call_count;
        return false;
    };
    dispatch_wm_close(handler);
    dispatch_wm_close(handler);
    dispatch_wm_close(handler);
    EXPECT_EQ(call_count, 3);
}
