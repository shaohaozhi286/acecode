// ThemePalette 基本完备性 + 全局切换测试。
// 验证两套调色板的每个槽位不为默认构造值,以及 swap 热切换工作正常。

#include <gtest/gtest.h>

#include "tui/theme_palette.hpp"

using namespace acecode::tui;

namespace {

// ftxui::Color 默认构造是 Default(无色),用 == Default 判断槽位是否被赋值
bool is_default(const ftxui::Color& c) {
    return c == ftxui::Color::Default;
}

// 遍历调色板所有 Color 槽位,确认没有遗漏赋值
void assert_all_slots_assigned(const ThemePalette& p) {
    // ui
    EXPECT_FALSE(is_default(p.ui.text_primary)) << p.name << " ui.text_primary";
    EXPECT_FALSE(is_default(p.ui.text_muted))   << p.name << " ui.text_muted";
    EXPECT_FALSE(is_default(p.ui.text_secondary))
        << p.name << " ui.text_secondary";
    EXPECT_FALSE(is_default(p.ui.text_dim))     << p.name << " ui.text_dim";
    EXPECT_FALSE(is_default(p.ui.accent))        << p.name << " ui.accent";
    EXPECT_FALSE(is_default(p.ui.accent_alt))    << p.name << " ui.accent_alt";
    EXPECT_FALSE(is_default(p.ui.border))        << p.name << " ui.border";
    EXPECT_FALSE(is_default(p.ui.selection_fg))  << p.name << " ui.selection_fg";
    EXPECT_FALSE(is_default(p.ui.selection_bg))  << p.name << " ui.selection_bg";
    EXPECT_FALSE(is_default(p.ui.badge_fg))      << p.name << " ui.badge_fg";
    EXPECT_FALSE(is_default(p.ui.badge_bg))      << p.name << " ui.badge_bg";
    EXPECT_FALSE(is_default(p.ui.queued_bg))     << p.name << " ui.queued_bg";

    // diff
    EXPECT_FALSE(is_default(p.diff.bg_added_line))   << p.name << " diff.bg_added_line";
    EXPECT_FALSE(is_default(p.diff.bg_added_word))   << p.name << " diff.bg_added_word";
    EXPECT_FALSE(is_default(p.diff.bg_removed_line)) << p.name << " diff.bg_removed_line";
    EXPECT_FALSE(is_default(p.diff.bg_removed_word)) << p.name << " diff.bg_removed_word";
    EXPECT_FALSE(is_default(p.diff.gutter))          << p.name << " diff.gutter";
    EXPECT_FALSE(is_default(p.diff.line_text))       << p.name << " diff.line_text";

    // syntax
    EXPECT_FALSE(is_default(p.syntax.keyword))      << p.name << " syntax.keyword";
    EXPECT_FALSE(is_default(p.syntax.string))       << p.name << " syntax.string";
    EXPECT_FALSE(is_default(p.syntax.number))       << p.name << " syntax.number";
    EXPECT_FALSE(is_default(p.syntax.comment))      << p.name << " syntax.comment";
    EXPECT_FALSE(is_default(p.syntax.type))         << p.name << " syntax.type";
    EXPECT_FALSE(is_default(p.syntax.preproc))      << p.name << " syntax.preproc";
    EXPECT_FALSE(is_default(p.syntax.function))     << p.name << " syntax.function";
    EXPECT_FALSE(is_default(p.syntax.op))           << p.name << " syntax.op";
    EXPECT_FALSE(is_default(p.syntax.default_text)) << p.name << " syntax.default_text";

    // markdown
    EXPECT_FALSE(is_default(p.markdown.code_span))       << p.name << " markdown.code_span";
    EXPECT_FALSE(is_default(p.markdown.link))            << p.name << " markdown.link";
    EXPECT_FALSE(is_default(p.markdown.bold))            << p.name << " markdown.bold";
    EXPECT_FALSE(is_default(p.markdown.italic))          << p.name << " markdown.italic";
    EXPECT_FALSE(is_default(p.markdown.heading))         << p.name << " markdown.heading";
    EXPECT_FALSE(is_default(p.markdown.block_code_text)) << p.name << " markdown.block_code_text";
    EXPECT_FALSE(is_default(p.markdown.block_quote))     << p.name << " markdown.block_quote";
    EXPECT_FALSE(is_default(p.markdown.list_marker))     << p.name << " markdown.list_marker";

    // semantic
    EXPECT_FALSE(is_default(p.semantic.success)) << p.name << " semantic.success";
    EXPECT_FALSE(is_default(p.semantic.warning)) << p.name << " semantic.warning";
    EXPECT_FALSE(is_default(p.semantic.error))   << p.name << " semantic.error";
    EXPECT_FALSE(is_default(p.semantic.info))    << p.name << " semantic.info";
}

} // namespace

// 场景:dark 调色板所有槽位都已赋值,防止新增字段时漏初始化
TEST(ThemePalette, DarkPaletteAllSlotsAssigned) {
    auto p = make_dark_palette();
    EXPECT_EQ(p.name, "dark");
    assert_all_slots_assigned(p);
}

// 场景:light 调色板所有槽位都已赋值
TEST(ThemePalette, LightPaletteAllSlotsAssigned) {
    auto p = make_light_palette();
    EXPECT_EQ(p.name, "light");
    assert_all_slots_assigned(p);
}

// 场景:init_theme_palette("dark") 后 theme() 返回 dark
TEST(ThemePalette, InitDarkReturnsCorrectName) {
    init_theme_palette("dark");
    EXPECT_EQ(current_theme_name(), "dark");
    EXPECT_EQ(theme().name, "dark");
}

// 场景:init_theme_palette("light") 后 theme() 返回 light
TEST(ThemePalette, InitLightReturnsCorrectName) {
    init_theme_palette("light");
    EXPECT_EQ(current_theme_name(), "light");
    EXPECT_EQ(theme().name, "light");
}

// 场景:swap 热切换 — dark → light → dark,每次 theme() 立刻反映
TEST(ThemePalette, SwapTogglesBetweenThemes) {
    init_theme_palette("dark");
    EXPECT_EQ(theme().name, "dark");

    swap_theme_palette("light");
    EXPECT_EQ(theme().name, "light");

    swap_theme_palette("dark");
    EXPECT_EQ(theme().name, "dark");
}

// 场景:未知名称 fallback 到 dark(保护性默认)
TEST(ThemePalette, UnknownNameFallsToDark) {
    init_theme_palette("nonexistent");
    EXPECT_EQ(theme().name, "dark");
}

// 场景:dark 和 light 的主强调色不同,确认不是复制粘贴漏改
TEST(ThemePalette, DarkAndLightAccentsDiffer) {
    auto d = make_dark_palette();
    auto l = make_light_palette();
    EXPECT_NE(d.ui.accent, l.ui.accent);
    EXPECT_NE(d.ui.text_primary, l.ui.text_primary);
    EXPECT_EQ(d.ui.text_secondary, Color::RGB(160, 160, 160));
    EXPECT_EQ(l.ui.text_secondary, Color::RGB(100, 100, 100));
    EXPECT_NE(d.ui.text_secondary, d.ui.text_dim);
    EXPECT_NE(l.ui.text_secondary, l.ui.text_dim);
    EXPECT_NE(d.diff.bg_added_line, l.diff.bg_added_line);
}
