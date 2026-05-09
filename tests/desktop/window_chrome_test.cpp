// 覆盖 src/desktop/window_chrome.cpp。无标题栏窗口依赖这些纯逻辑把
// Win32 命中测试映射到 resize / caption / client,避免去掉原生标题栏后
// 用户无法拖动或调整大小。

#include <gtest/gtest.h>

#include "desktop/window_chrome.hpp"

using acecode::desktop::FramelessHitTestArea;
using acecode::desktop::FramelessHitTestInput;
using acecode::desktop::classify_frameless_hit_test;
using acecode::desktop::frameless_resize_border;
using acecode::desktop::parse_resize_direction;

namespace {

FramelessHitTestInput base_input() {
    FramelessHitTestInput in;
    in.width = 1280;
    in.height = 820;
    in.frame_x = 8;
    in.frame_y = 8;
    in.padding = 4;
    in.drag_height = 44;
    return in;
}

} // namespace

// 场景: resize 边框宽度至少为 1,避免 DPI/测试输入异常时完全失去 resize 区。
TEST(DesktopWindowChrome, ResizeBorderHasMinimum) {
    EXPECT_EQ(frameless_resize_border(0, 0), 1);
    EXPECT_EQ(frameless_resize_border(8, 4), 12);
}

// 场景: 非最大化窗口四角必须优先命中 resize,不能被顶栏拖拽区吞掉。
TEST(DesktopWindowChrome, CornersPreferResizeOverCaption) {
    auto in = base_input();
    in.x = 2;
    in.y = 2;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::TopLeft);

    in.x = 1278;
    in.y = 2;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::TopRight);

    in.x = 2;
    in.y = 818;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::BottomLeft);

    in.x = 1278;
    in.y = 818;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::BottomRight);
}

// 场景: 顶部 resize 带下面的空白顶栏区域用于拖动窗口。
TEST(DesktopWindowChrome, TopBarAreaActsAsCaption) {
    auto in = base_input();
    in.x = 240;
    in.y = 24;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::Caption);
}

// 场景: 顶栏以下的区域仍然是普通 client,让 WebView 正常接收点击。
TEST(DesktopWindowChrome, ContentAreaIsClient) {
    auto in = base_input();
    in.x = 240;
    in.y = 80;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::Client);
}

// 场景: 最大化时不返回 resize 区,避免屏幕边缘出现错误拖拽/缩放行为。
TEST(DesktopWindowChrome, MaximizedWindowSuppressesResizeEdges) {
    auto in = base_input();
    in.maximized = true;
    in.x = 2;
    in.y = 2;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::Caption);

    in.y = 818;
    EXPECT_EQ(classify_frameless_hit_test(in), FramelessHitTestArea::Client);
}

// 场景: 前端 strip 在 mousedown 时通过 aceDesktop_startWindowResize 把方向
// 字符串送进 native;parse_resize_direction 是这条桥的"必经哨兵",所有合法
// 边/角必须能正确翻译,无效值必须返回 nullopt 而不是 fallback。
TEST(DesktopWindowChrome, ParseResizeDirectionAcceptsAllEightEdgesAndCorners) {
    EXPECT_EQ(parse_resize_direction("top"),          FramelessHitTestArea::Top);
    EXPECT_EQ(parse_resize_direction("bottom"),       FramelessHitTestArea::Bottom);
    EXPECT_EQ(parse_resize_direction("left"),         FramelessHitTestArea::Left);
    EXPECT_EQ(parse_resize_direction("right"),        FramelessHitTestArea::Right);
    EXPECT_EQ(parse_resize_direction("top-left"),     FramelessHitTestArea::TopLeft);
    EXPECT_EQ(parse_resize_direction("top-right"),    FramelessHitTestArea::TopRight);
    EXPECT_EQ(parse_resize_direction("bottom-left"),  FramelessHitTestArea::BottomLeft);
    EXPECT_EQ(parse_resize_direction("bottom-right"), FramelessHitTestArea::BottomRight);
}

// 场景: caller 打错字 / 大小写不一致 / 空串 / 注入"client"或"caption"试图
// 让 native 当成拖动 — 都必须返回 nullopt,WebHost::start_window_resize 才能
// 安全地拒绝调用。回归点:不要为了"宽松"加大小写归一化或下划线/空格容忍,
// 否则前端拼错字会被 native 静默接受、产生难以排查的怪异 resize。
TEST(DesktopWindowChrome, ParseResizeDirectionRejectsInvalidStrings) {
    EXPECT_FALSE(parse_resize_direction(""));
    EXPECT_FALSE(parse_resize_direction("Top"));
    EXPECT_FALSE(parse_resize_direction("TOP"));
    EXPECT_FALSE(parse_resize_direction("top_left"));
    EXPECT_FALSE(parse_resize_direction("topleft"));
    EXPECT_FALSE(parse_resize_direction("top "));
    EXPECT_FALSE(parse_resize_direction("client"));
    EXPECT_FALSE(parse_resize_direction("caption"));
    EXPECT_FALSE(parse_resize_direction("ne"));
    EXPECT_FALSE(parse_resize_direction("northwest"));
}