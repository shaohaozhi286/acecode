// 终端背景色检测的纯函数测试。
// 通过 detect_terminal_theme_with() 注入桩回调,不碰真实终端 I/O。

#include <gtest/gtest.h>

#include "utils/terminal_theme_detect.hpp"

#include <optional>
#include <string>

using namespace acecode;

namespace {

auto no_osc11() {
    return []() -> std::optional<std::string> { return std::nullopt; };
}
auto no_env() {
    return [](const char*) -> std::optional<std::string> { return std::nullopt; };
}
auto no_terminal_background() {
    return []() -> std::optional<bool> { return std::nullopt; };
}

} // namespace

// ---- parse_osc11_luminance ----

// 场景:标准 4 位 hex 格式(如 iTerm2 / Windows Terminal 返回的值)
TEST(ParseOsc11, StandardFourDigitHex) {
    // rgb:0000/0000/0000 = 纯黑 → 亮度 0
    auto lum = parse_osc11_luminance("rgb:0000/0000/0000");
    ASSERT_TRUE(lum.has_value());
    EXPECT_NEAR(*lum, 0.0, 0.001);
}

// 场景:纯白背景 → 亮度接近 1
TEST(ParseOsc11, PureWhite) {
    auto lum = parse_osc11_luminance("rgb:ffff/ffff/ffff");
    ASSERT_TRUE(lum.has_value());
    EXPECT_NEAR(*lum, 1.0, 0.001);
}

// 场景:2 位 hex(部分终端如 xterm 返回缩写格式)
TEST(ParseOsc11, TwoDigitHex) {
    // rgb:ff/ff/ff → 等价于纯白
    auto lum = parse_osc11_luminance("rgb:ff/ff/ff");
    ASSERT_TRUE(lum.has_value());
    EXPECT_NEAR(*lum, 1.0, 0.001);
}

// 场景:1 位 hex 格式
TEST(ParseOsc11, SingleDigitHex) {
    // rgb:0/0/0 = 纯黑
    auto lum = parse_osc11_luminance("rgb:0/0/0");
    ASSERT_TRUE(lum.has_value());
    EXPECT_NEAR(*lum, 0.0, 0.001);
}

// 场景:3 位 hex 格式
TEST(ParseOsc11, ThreeDigitHex) {
    auto lum = parse_osc11_luminance("rgb:fff/fff/fff");
    ASSERT_TRUE(lum.has_value());
    EXPECT_NEAR(*lum, 1.0, 0.001);
}

// 场景:典型暗色终端背景(Solarized Dark #002b36 → RGB ~0,43,54)
TEST(ParseOsc11, SolarizedDarkBackground) {
    // 4 位 hex: 0000/2b2b/3636
    auto lum = parse_osc11_luminance("rgb:0000/2b2b/3636");
    ASSERT_TRUE(lum.has_value());
    EXPECT_LT(*lum, 0.5); // 暗色背景
}

// 场景:典型亮色终端背景(Solarized Light #fdf6e3 → RGB ~253,246,227)
TEST(ParseOsc11, SolarizedLightBackground) {
    auto lum = parse_osc11_luminance("rgb:fdfd/f6f6/e3e3");
    ASSERT_TRUE(lum.has_value());
    EXPECT_GT(*lum, 0.5); // 亮色背景
}

// 场景:格式不合法 → nullopt
TEST(ParseOsc11, MalformedReturnsNullopt) {
    EXPECT_FALSE(parse_osc11_luminance("").has_value());
    EXPECT_FALSE(parse_osc11_luminance("garbage").has_value());
    EXPECT_FALSE(parse_osc11_luminance("rgb:").has_value());
    EXPECT_FALSE(parse_osc11_luminance("rgb:ff/ff").has_value()); // 只有两个分量
    EXPECT_FALSE(parse_osc11_luminance("rgb:zzzz/0000/0000").has_value()); // 非法 hex
}

// ---- parse_colorfgbg ----

// 场景:ANSI 背景索引 0-6 算暗色,7-15 算亮色
TEST(AnsiBackgroundIndex, DarkAndLightBuckets) {
    EXPECT_EQ(theme_from_ansi_background_index(0), DetectedTheme::dark);
    EXPECT_EQ(theme_from_ansi_background_index(6), DetectedTheme::dark);
    EXPECT_EQ(theme_from_ansi_background_index(7), DetectedTheme::light);
    EXPECT_EQ(theme_from_ansi_background_index(15), DetectedTheme::light);
}

// 场景:典型暗色终端 COLORFGBG="15;0"(前景白,背景黑=ANSI 0)
TEST(ParseColorFgBg, DarkBackground) {
    auto result = parse_colorfgbg("15;0");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, DetectedTheme::dark);
}

// 场景:典型亮色终端 COLORFGBG="0;15"(前景黑,背景白=ANSI 15)
TEST(ParseColorFgBg, LightBackground) {
    auto result = parse_colorfgbg("0;15");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, DetectedTheme::light);
}

// 场景:三段式(rxvt 扩展)"0;default;15"——取最后一段
TEST(ParseColorFgBg, ThreeSegmentsLastIsBackground) {
    auto result = parse_colorfgbg("0;default;15");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, DetectedTheme::light);
}

// 场景:边界值 ANSI 6 仍算暗色(≤6)
TEST(ParseColorFgBg, Ansi6IsDark) {
    auto result = parse_colorfgbg("15;6");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, DetectedTheme::dark);
}

// 场景:ANSI 7(亮灰)算亮色(>6)
TEST(ParseColorFgBg, Ansi7IsLight) {
    auto result = parse_colorfgbg("0;7");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, DetectedTheme::light);
}

// 场景:空值或非数字 → nullopt
TEST(ParseColorFgBg, InvalidReturnsNullopt) {
    EXPECT_FALSE(parse_colorfgbg("").has_value());
    EXPECT_FALSE(parse_colorfgbg("abc").has_value());
}

// ---- detect_terminal_theme_with (回退链) ----

// 场景:OSC 11 返回暗色背景 → 直接采用,不走后续
TEST(DetectThemeWith, Osc11DarkWins) {
    auto result = detect_terminal_theme_with(
        []() -> std::optional<std::string> { return "rgb:0000/0000/0000"; },
        no_env(),
        []() -> std::optional<bool> { return false; }  // 后续说亮色 — 应被忽略
    );
    EXPECT_EQ(result, DetectedTheme::dark);
}

// 场景:OSC 11 返回亮色背景
TEST(DetectThemeWith, Osc11LightWins) {
    auto result = detect_terminal_theme_with(
        []() -> std::optional<std::string> { return "rgb:ffff/ffff/ffff"; },
        no_env(),
        no_terminal_background()
    );
    EXPECT_EQ(result, DetectedTheme::light);
}

// 场景:OSC 11 不可用,回退到 COLORFGBG
TEST(DetectThemeWith, FallbackToColorFgBg) {
    auto result = detect_terminal_theme_with(
        no_osc11(),
        [](const char* name) -> std::optional<std::string> {
            if (std::string(name) == "COLORFGBG") return "0;15";
            return std::nullopt;
        },
        no_terminal_background()
    );
    EXPECT_EQ(result, DetectedTheme::light);
}

// 场景:OSC 11 + COLORFGBG 都不可用,回退到平台终端背景=false(亮色)
TEST(DetectThemeWith, FallbackToTerminalBackgroundLight) {
    auto result = detect_terminal_theme_with(
        no_osc11(),
        no_env(),
        []() -> std::optional<bool> { return false; } // 终端亮色背景
    );
    EXPECT_EQ(result, DetectedTheme::light);
}

// 场景:OSC 11 + COLORFGBG 都不可用,平台终端背景为暗色
TEST(DetectThemeWith, FallbackToTerminalBackgroundDark) {
    auto result = detect_terminal_theme_with(
        no_osc11(),
        no_env(),
        []() -> std::optional<bool> { return true; } // 终端暗色背景
    );
    EXPECT_EQ(result, DetectedTheme::dark);
}

// 场景:三条路径全失败 → 默认 dark
TEST(DetectThemeWith, AllFailDefaultDark) {
    auto result = detect_terminal_theme_with(
        no_osc11(),
        no_env(),
        no_terminal_background()
    );
    EXPECT_EQ(result, DetectedTheme::dark);
}

// 场景:OSC 11 返回但解析失败(乱码) → 回退到 COLORFGBG
TEST(DetectThemeWith, Osc11GarbageFallsThrough) {
    auto result = detect_terminal_theme_with(
        []() -> std::optional<std::string> { return "not-rgb-format"; },
        [](const char* name) -> std::optional<std::string> {
            if (std::string(name) == "COLORFGBG") return "15;0";
            return std::nullopt;
        },
        no_terminal_background()
    );
    EXPECT_EQ(result, DetectedTheme::dark);
}
