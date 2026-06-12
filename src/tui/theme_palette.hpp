#pragma once

// TUI 调色板 — 按组件分组的语义化颜色槽位。
//
// dark / light 两套预置调色板,由 init_theme_palette() 在 FTXUI 初始化之前
// 设置全局实例;运行期可通过 swap_theme_palette() 热切换(下一帧立刻生效)。
//
// 调用站点统一用 theme().ui.accent 等语义名取色,不再硬编码 Color::Yellow。

#include <ftxui/screen/color.hpp>

#include <atomic>
#include <string>

namespace acecode::tui {

using ftxui::Color;

// ---- 组件分组 ----

struct UiColors {
    Color text_primary;     // 强调文字(标题、粗体)
    Color text_muted;       // 普通文字
    Color text_dim;         // 暗淡文字(分隔符、辅助信息)
    Color accent;           // 主强调色(工具名、关键字高亮)
    Color accent_alt;       // 次强调色(计时器、token 统计)
    Color border;           // 边框、下拉面板
    Color selection_fg;     // 选中项前景
    Color selection_bg;     // 选中项背景
    Color badge_fg;         // 标签前景(attachment badge)
    Color badge_bg;         // 标签背景
    Color queued_bg;        // 排队消息背景
};

struct DiffColors {
    Color bg_added_line;
    Color bg_added_word;
    Color bg_removed_line;
    Color bg_removed_word;
    Color gutter;           // 行号槽
    Color line_text;        // diff 行内文字
};

struct SyntaxColors {
    Color keyword;
    Color string;
    Color number;
    Color comment;
    Color type;
    Color preproc;
    Color function;
    Color op;
    Color default_text;
};

struct MarkdownColors {
    Color code_span;        // 行内代码
    Color link;
    Color bold;
    Color italic;
    Color heading;          // 标题(h1-h6)
    Color block_code_text;  // 代码块内文字
    Color block_quote;      // 引用栏
    Color list_marker;      // 列表标记(·、1.、-)
};

struct SemanticColors {
    Color success;          // +行、完成
    Color warning;          // 中等负载、注意
    Color error;            // 删除、高负载
    Color info;             // 信息性文字
};

// ---- 完整调色板 ----

struct ThemePalette {
    std::string name;       // "dark" 或 "light"

    UiColors       ui;
    DiffColors     diff;
    SyntaxColors   syntax;
    MarkdownColors markdown;
    SemanticColors semantic;
};

// 两套预置调色板工厂。
ThemePalette make_dark_palette();
ThemePalette make_light_palette();

// 全局访问器 — 返回当前活跃调色板的 const 引用。
// 初次调用前必须先 init_theme_palette()。
const ThemePalette& theme();

// 启动时设置全局调色板。name = "dark" | "light"。
void init_theme_palette(const std::string& name);

// 运行期热切换(立刻替换全局指针,FTXUI 下帧取新值)。
void swap_theme_palette(const std::string& name);

// 当前活跃调色板名称。
const std::string& current_theme_name();

} // namespace acecode::tui
