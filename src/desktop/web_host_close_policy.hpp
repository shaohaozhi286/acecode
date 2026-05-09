#pragma once

// WebHost 的"关窗 / 退出"派发策略 — 提取为纯函数让 unit test 不需要真窗口就能覆盖。
// 实际 WndProc 调用在 web_host.cpp 里(web_host.cpp 因为 webview 依赖被排除在
// acecode_testable 外,所以策略函数必须独立 TU)。
//
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 2。

#include <functional>

namespace acecode::desktop {

// WM_CLOSE 派发结果。
enum class CloseDispatch {
    // close handler 返回 true,关窗请求被消化(典型:隐藏到托盘),保留窗口。
    ConsumedByHandler,
    // 没有 handler / handler 返回 false → 走原 DestroyWindow 退出路径。
    FallthroughToDestroy,
};

// 纯函数:决定 WM_CLOSE 时该怎么走。
inline CloseDispatch dispatch_wm_close(const std::function<bool()>& handler) {
    if (handler && handler()) {
        return CloseDispatch::ConsumedByHandler;
    }
    return CloseDispatch::FallthroughToDestroy;
}

// kRequestQuitMsg(WM_USER + 0x10)固定走 DestroyWindow,绕过 close handler。
// 暴露成函数主要为了文档与一致性 — 真实路径在 host_window_proc 里直接 DestroyWindow,
// 这里返回 enum 让测试断言 "request_quit 永远 destroy" 这一不变量。
inline CloseDispatch dispatch_request_quit() {
    return CloseDispatch::FallthroughToDestroy;
}

} // namespace acecode::desktop
