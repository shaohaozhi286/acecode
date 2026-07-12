// window_background 纯逻辑单测。
//
// 背景:桌面壳快速拖动窗口放大时,WebView2 合成器异步跟不上,新暴露区域由
// 宿主窗口类背景刷打底 — 打底色经三条通道下发(窗口类刷子 / webview_widget
// 类刷子 / WEBVIEW2_DEFAULT_BACKGROUND_COLOR 环境变量 + put_DefaultBackgroundColor),
// 颜色字符串来自前端 bridge。这里覆盖解析哨兵与环境变量格式两块纯逻辑。

#include "desktop/window_background.hpp"

#include <gtest/gtest.h>

namespace {

using acecode::desktop::WindowBackgroundColor;
using acecode::desktop::kDefaultWindowBackground;
using acecode::desktop::parse_window_background_color;
using acecode::desktop::webview2_default_background_env_value;

// 场景:前端 ThemeProvider 推送浅色主题 body 底色 "#f5f5f2"(globals.css
// --ace-bg 的字面量形态)。期望:解析成功且 RGB 分量逐字节对应。
TEST(WindowBackgroundParse, LowercaseHexWithHashParses) {
    auto color = parse_window_background_color("#f5f5f2");
    ASSERT_TRUE(color.has_value());
    EXPECT_EQ(*color, (WindowBackgroundColor{0xF5, 0xF5, 0xF2}));
}

// 场景:调用方省略 '#' 前缀(bridge 是公开 window 函数,形态无法完全约束)。
// 期望:裸 6 位 hex 同样接受 — '#' 是可选前缀而不是必须。
TEST(WindowBackgroundParse, BareHexWithoutHashParses) {
    auto color = parse_window_background_color("0F0F0f");
    ASSERT_TRUE(color.has_value());
    EXPECT_EQ(*color, (WindowBackgroundColor{0x0F, 0x0F, 0x0F}));
}

// 场景:大小写混合(不同 CSS 书写习惯)。期望:大小写不敏感。
TEST(WindowBackgroundParse, MixedCaseParses) {
    auto color = parse_window_background_color("#FaFaF8");
    ASSERT_TRUE(color.has_value());
    EXPECT_EQ(*color, (WindowBackgroundColor{0xFA, 0xFA, 0xF8}));
}

// 场景:非法形态 — 空串 / 只有 '#' / #RGB 短形 / 7 位 / 8 位带 alpha /
// 含非 hex 字符 / 前后空白 / rgb() 函数形态。期望:全部拒绝(nullopt),
// 与 parse_resize_direction 同款哨兵策略 — 非法输入不猜,防止前端笔误把
// 窗口涂成意外颜色;规范化(trim / #RGB 展开)是前端纯函数的职责。
TEST(WindowBackgroundParse, MalformedInputsRejected) {
    EXPECT_FALSE(parse_window_background_color("").has_value());
    EXPECT_FALSE(parse_window_background_color("#").has_value());
    EXPECT_FALSE(parse_window_background_color("#fff").has_value());
    EXPECT_FALSE(parse_window_background_color("#f5f5f21").has_value());
    EXPECT_FALSE(parse_window_background_color("#ff00ff00").has_value());
    EXPECT_FALSE(parse_window_background_color("#f5g5f2").has_value());
    EXPECT_FALSE(parse_window_background_color(" #f5f5f2").has_value());
    EXPECT_FALSE(parse_window_background_color("#f5f5f2 ").has_value());
    EXPECT_FALSE(parse_window_background_color("rgb(245, 245, 242)").has_value());
}

// 场景:启动时把默认打底色写进 WEBVIEW2_DEFAULT_BACKGROUND_COLOR 环境变量。
// 期望:输出 "FF" alpha 前缀 + 大写 RGB 的完整 8 位 hex — WebView2 官方文档
// 规定不足 8 位会被前补 00 解释成全透明,漏掉 alpha 前缀 = 打底色悄悄失效,
// 这是本格式函数存在的全部意义。
TEST(WindowBackgroundEnvValue, EmitsAlphaPrefixedEightDigitHex) {
    EXPECT_EQ(webview2_default_background_env_value({0xF5, 0xF5, 0xF2}), "FFF5F5F2");
}

// 场景:分量 < 0x10 时(暗色主题 #0f0f0f)。期望:每字节两位、高位补零 —
// 输出宽度恒 8,不会因为小分量塌成 7 位而触发 WebView2 的补 00 透明解释。
TEST(WindowBackgroundEnvValue, ZeroPadsSmallComponents) {
    EXPECT_EQ(webview2_default_background_env_value({0x0F, 0x0F, 0x0F}), "FF0F0F0F");
    EXPECT_EQ(webview2_default_background_env_value({0x00, 0x00, 0x00}), "FF000000");
}

// 场景:启动默认色常量应与前端浅色主题 body 底色(globals.css --ace-bg
// #f5f5f2)一致。期望:常量本身 + 环境变量形态双向锁死,改动任何一侧时
// 该测试提醒同步另一侧(CSS 改主题底色时这里会红)。
TEST(WindowBackgroundDefault, MatchesFrontendLightThemeBody) {
    EXPECT_EQ(kDefaultWindowBackground, (WindowBackgroundColor{0xF5, 0xF5, 0xF2}));
    EXPECT_EQ(webview2_default_background_env_value(kDefaultWindowBackground), "FFF5F5F2");
}

} // namespace
