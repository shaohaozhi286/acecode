#include "theme_palette.hpp"

#include <cassert>

namespace acecode::tui {

// ---- Dark 调色板(黑底终端,即现有配色) ----

ThemePalette make_dark_palette() {
    ThemePalette p;
    p.name = "dark";

    p.ui = {
        /* text_primary  */ Color::White,
        /* text_muted    */ Color::GrayLight,
        /* text_secondary */ Color::RGB(160, 160, 160),
        /* text_dim      */ Color::GrayDark,
        /* accent        */ Color::Yellow,
        /* accent_alt    */ Color::CyanLight,
        /* border        */ Color::Cyan,
        /* selection_fg  */ Color::White,
        /* selection_bg  */ Color::RGB(0, 80, 120),
        /* badge_fg      */ Color::Black,
        /* badge_bg      */ Color::CyanLight,
        /* queued_bg     */ Color::RGB(128, 96, 0),
    };

    p.diff = {
        /* bg_added_line   */ Color::RGB(30, 70, 30),
        /* bg_added_word   */ Color::RGB(40, 130, 40),
        /* bg_removed_line */ Color::RGB(90, 30, 30),
        /* bg_removed_word */ Color::RGB(180, 40, 40),
        /* gutter          */ Color::GrayDark,
        /* line_text       */ Color::GrayLight,
    };

    p.syntax = {
        /* keyword      */ Color::BlueLight,
        /* string       */ Color::Green,
        /* number       */ Color::Yellow,
        /* comment      */ Color::GrayDark,
        /* type         */ Color::Cyan,
        /* preproc      */ Color::Cyan,       // 工具发起行 → 与 ACE logo 同青蓝
        /* function     */ Color::Yellow,
        /* op           */ Color::White,
        /* default_text */ Color::GrayLight,
    };

    p.markdown = {
        /* code_span       */ Color::Yellow,
        /* link            */ Color::BlueLight,
        /* bold            */ Color::White,
        /* italic          */ Color::GrayLight,
        /* heading         */ Color::CyanLight,
        /* block_code_text */ Color::GrayLight,
        /* block_quote     */ Color::GrayDark,
        /* list_marker     */ Color::White,
    };

    p.semantic = {
        /* success */ Color::GreenLight,
        /* warning */ Color::Yellow,
        /* error   */ Color::RedLight,
        /* info    */ Color::CyanLight,
    };

    return p;
}

// ---- Light 调色板(白底终端) ----
// 配色参考 VS Code Light+ 主题和 GitHub 浅色代码高亮。
// 原则:深色文字 + 高饱和度强调色 + 浅色 diff 背景。

ThemePalette make_light_palette() {
    ThemePalette p;
    p.name = "light";

    p.ui = {
        /* text_primary  */ Color::Black,
        /* text_muted    */ Color::RGB(80, 80, 80),
        /* text_secondary */ Color::RGB(100, 100, 100),
        /* text_dim      */ Color::RGB(140, 140, 140),
        /* accent        */ Color::RGB(160, 100, 0),
        /* accent_alt    */ Color::RGB(0, 120, 160),
        /* border        */ Color::RGB(0, 100, 140),
        /* selection_fg  */ Color::Black,
        /* selection_bg  */ Color::RGB(180, 215, 255),
        /* badge_fg      */ Color::White,
        /* badge_bg      */ Color::RGB(0, 100, 140),
        /* queued_bg     */ Color::RGB(200, 160, 40),
    };

    p.diff = {
        /* bg_added_line   */ Color::RGB(210, 255, 210),
        /* bg_added_word   */ Color::RGB(160, 240, 160),
        /* bg_removed_line */ Color::RGB(255, 215, 215),
        /* bg_removed_word */ Color::RGB(255, 150, 150),
        /* gutter          */ Color::RGB(140, 140, 140),
        /* line_text       */ Color::RGB(60, 60, 60),
    };

    p.syntax = {
        /* keyword      */ Color::RGB(0, 0, 180),
        /* string       */ Color::RGB(0, 128, 0),
        /* number       */ Color::RGB(170, 100, 0),
        /* comment      */ Color::RGB(0, 128, 0),
        /* type         */ Color::RGB(0, 110, 150),
        /* preproc      */ Color::RGB(0, 100, 140),  // 工具发起行 → 与 ACE logo 同青蓝
        /* function     */ Color::RGB(120, 80, 0),
        /* op           */ Color::Black,
        /* default_text */ Color::RGB(60, 60, 60),
    };

    p.markdown = {
        /* code_span       */ Color::RGB(180, 80, 0),
        /* link            */ Color::RGB(0, 80, 200),
        /* bold            */ Color::Black,
        /* italic          */ Color::RGB(80, 80, 80),
        /* heading         */ Color::RGB(0, 100, 150),
        /* block_code_text */ Color::RGB(50, 50, 50),
        /* block_quote     */ Color::RGB(120, 120, 120),
        /* list_marker     */ Color::RGB(40, 40, 40),
    };

    p.semantic = {
        /* success */ Color::RGB(0, 130, 0),
        /* warning */ Color::RGB(180, 120, 0),
        /* error   */ Color::RGB(200, 0, 0),
        /* info    */ Color::RGB(0, 120, 160),
    };

    return p;
}

// ---- 全局实例 ----

static ThemePalette g_dark  = make_dark_palette();
static ThemePalette g_light = make_light_palette();
static std::atomic<const ThemePalette*> g_active{&g_dark};

const ThemePalette& theme() {
    return *g_active.load(std::memory_order_acquire);
}

void init_theme_palette(const std::string& name) {
    if (name == "light") {
        g_active.store(&g_light, std::memory_order_release);
    } else {
        g_active.store(&g_dark, std::memory_order_release);
    }
}

void swap_theme_palette(const std::string& name) {
    init_theme_palette(name);
}

const std::string& current_theme_name() {
    return g_active.load(std::memory_order_acquire)->name;
}

} // namespace acecode::tui
