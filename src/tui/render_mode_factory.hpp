#pragma once

// FTXUI ScreenInteractive 工厂 — 把 ScreenRenderMode 决策落到具体的
// FTXUI 调用上。和 render_mode.hpp 拆开是为了让纯函数 decide_render_mode
// 保持无 FTXUI 依赖,可以在 acecode_testable 里直接用。
//
// 实现见 render_mode.cpp,只在 main.cpp 这种已经持有 FTXUI 依赖的调用站点
// include。

#include <ftxui/component/screen_interactive.hpp>

#include "render_mode.hpp"

namespace acecode::tui {

// AltScreen → ScreenInteractive::Fullscreen()(\033[?1049h 切到备用屏)
// TerminalOutput → ScreenInteractive::TerminalOutput()(原历史行为)
ftxui::ScreenInteractive make_screen_interactive(ScreenRenderMode mode);

} // namespace acecode::tui
