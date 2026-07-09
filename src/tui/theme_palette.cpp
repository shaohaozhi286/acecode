#include "theme_palette.hpp"

#include <cassert>

namespace acecode::tui {

// ---- Dark 调色板(黑底终端,基于 Tokyo Night 调校) ----
// 设计原则(见「TUI 配色方案」):正文用柔白而非纯白,避免黑白极端对比;
// 工具发起(紫 #bb9af7)/ 工具返回(青蓝 #7dcfff)/ 成功(草绿 #9ece6a)
// 三向区分;错误用珊瑚红;数字/耗时统一用橙,弱化文本用灰蓝。
// FTXUI 的 Color::RGB 在不支持真彩的终端会自动落到最接近的基色。

ThemePalette make_dark_palette() {
    ThemePalette p;
    p.name = "dark";

    p.ui = {
        /* text_primary  */ Color::RGB(200, 208, 224),  // #c8d0e0 柔白
        /* text_muted    */ Color::RGB(169, 177, 214),  // #a9b1d6
        /* text_dim      */ Color::RGB(86, 95, 137),    // #565f89
        /* accent        */ Color::RGB(200, 208, 224),  // 柔白 — 主强调字保持白色系
        /* accent_alt    */ Color::RGB(255, 158, 100),  // #ff9e64 数字/耗时橙
        /* border        */ Color::RGB(200, 208, 224),  // 柔白 — 线框保持白色系
        /* selection_fg  */ Color::RGB(200, 208, 224),
        /* selection_bg  */ Color::RGB(54, 74, 130),    // #364a82
        /* badge_fg      */ Color::RGB(22, 22, 30),     // #16161e
        /* badge_bg      */ Color::RGB(125, 207, 255),  // #7dcfff
        /* queued_bg     */ Color::RGB(90, 70, 20),     // 暗琥珀
    };

    p.diff = {
        /* bg_added_line   */ Color::RGB(31, 45, 36),    // #1f2d24 低饱和绿底
        /* bg_added_word   */ Color::RGB(47, 77, 56),
        /* bg_removed_line */ Color::RGB(55, 34, 40),    // 低饱和红底
        /* bg_removed_word */ Color::RGB(90, 47, 56),
        /* gutter          */ Color::RGB(86, 95, 137),   // #565f89
        /* line_text       */ Color::RGB(169, 177, 214),
    };

    p.syntax = {
        /* keyword      */ Color::RGB(187, 154, 247),  // #bb9af7 紫
        /* string       */ Color::RGB(158, 206, 106),  // #9ece6a
        /* number       */ Color::RGB(255, 158, 100),  // #ff9e64
        /* comment      */ Color::RGB(86, 95, 137),    // #565f89
        /* type         */ Color::RGB(125, 207, 255),  // #7dcfff
        /* preproc      */ Color::RGB(187, 154, 247),  // #bb9af7(也是工具发起箭头色)
        /* function     */ Color::RGB(122, 162, 247),  // #7aa2f7
        /* op           */ Color::RGB(137, 221, 255),  // #89ddff
        /* default_text */ Color::RGB(169, 177, 214),
    };

    p.markdown = {
        /* code_span       */ Color::RGB(125, 207, 255),  // #7dcfff
        /* link            */ Color::RGB(122, 162, 247),  // #7aa2f7
        /* bold            */ Color::RGB(200, 208, 224),
        /* italic          */ Color::RGB(169, 177, 214),
        /* heading         */ Color::RGB(122, 162, 247),  // #7aa2f7
        /* block_code_text */ Color::RGB(169, 177, 214),
        /* block_quote     */ Color::RGB(86, 95, 137),
        /* list_marker     */ Color::RGB(200, 208, 224),
    };

    p.semantic = {
        /* success */ Color::RGB(158, 206, 106),  // #9ece6a 草绿
        /* warning */ Color::RGB(224, 175, 104),  // #e0af68 琥珀
        /* error   */ Color::RGB(247, 118, 142),  // #f7768e 珊瑚红
        /* info    */ Color::RGB(125, 207, 255),  // #7dcfff 青蓝(工具返回箭头)
    };

    return p;
}

// ---- Light 调色板(白底终端,基于 GitHub Light) ----
// 设计原则(见「TUI 配色方案」):同一语义角色在白底必须换压暗取值——
// 荧光绿/纯青在白底几乎不可读,成功用深绿 #1a7f37、路径用深蓝 #0550ae;
// 工具发起紫 #8250df / 工具返回蓝 #0969da 与深色主题的角色一一对应。

ThemePalette make_light_palette() {
    ThemePalette p;
    p.name = "light";

    p.ui = {
        /* text_primary  */ Color::RGB(36, 41, 47),     // #24292f
        /* text_muted    */ Color::RGB(87, 96, 106),    // #57606a
        /* text_dim      */ Color::RGB(139, 148, 158),  // #8b949e
        /* accent        */ Color::RGB(36, 41, 47),     // 墨色 — 白底上的"白色系"对应色
        /* accent_alt    */ Color::RGB(188, 76, 0),     // #bc4c00 数字/耗时橙棕
        /* border        */ Color::RGB(36, 41, 47),     // 墨色 — 线框同正文主色
        /* selection_fg  */ Color::RGB(36, 41, 47),
        /* selection_bg  */ Color::RGB(180, 215, 255),
        /* badge_fg      */ Color::RGB(255, 255, 255),
        /* badge_bg      */ Color::RGB(9, 105, 218),
        /* queued_bg     */ Color::RGB(234, 197, 79),   // #eac54f
    };

    p.diff = {
        /* bg_added_line   */ Color::RGB(230, 255, 236),  // #e6ffec GitHub diff 绿
        /* bg_added_word   */ Color::RGB(171, 242, 188),  // #abf2bc
        /* bg_removed_line */ Color::RGB(255, 235, 233),  // #ffebe9
        /* bg_removed_word */ Color::RGB(255, 178, 176),
        /* gutter          */ Color::RGB(139, 148, 158),
        /* line_text       */ Color::RGB(36, 41, 47),
    };

    p.syntax = {
        /* keyword      */ Color::RGB(130, 80, 223),  // #8250df 紫
        /* string       */ Color::RGB(10, 48, 105),   // #0a3069
        /* number       */ Color::RGB(188, 76, 0),    // #bc4c00
        /* comment      */ Color::RGB(110, 119, 129), // #6e7781
        /* type         */ Color::RGB(9, 105, 218),   // #0969da
        /* preproc      */ Color::RGB(130, 80, 223),  // #8250df(也是工具发起箭头色)
        /* function     */ Color::RGB(5, 80, 174),    // #0550ae
        /* op           */ Color::RGB(36, 41, 47),
        /* default_text */ Color::RGB(36, 41, 47),
    };

    p.markdown = {
        /* code_span       */ Color::RGB(9, 105, 218),   // #0969da
        /* link            */ Color::RGB(9, 105, 218),
        /* bold            */ Color::RGB(36, 41, 47),
        /* italic          */ Color::RGB(87, 96, 106),
        /* heading         */ Color::RGB(9, 105, 218),
        /* block_code_text */ Color::RGB(36, 41, 47),
        /* block_quote     */ Color::RGB(139, 148, 158),
        /* list_marker     */ Color::RGB(36, 41, 47),
    };

    p.semantic = {
        /* success */ Color::RGB(26, 127, 55),   // #1a7f37 深绿
        /* warning */ Color::RGB(154, 103, 0),   // #9a6700 压暗琥珀
        /* error   */ Color::RGB(207, 34, 46),   // #cf222e
        /* info    */ Color::RGB(9, 105, 218),   // #0969da 蓝(工具返回箭头)
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
