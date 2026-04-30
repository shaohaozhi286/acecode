#pragma once

// FTXUI 渲染模式决策 — 给 add-legacy-terminal-fallback 用。
//
// 把 main.cpp 里硬编码的 ScreenInteractive::TerminalOutput() / Fullscreen()
// 二选一抽到一个纯函数 + 一个工厂里:
//   - decide_render_mode(cfg, caps) — 纯函数,只看配置和能力探测结果,
//     无 FTXUI 依赖,可被 acecode_testable 直接 include 使用(同
//     picker_scroll.hpp 的 header-only pattern)。
//   - make_screen_interactive(mode) — 工厂,会调 FTXUI,实现在
//     render_mode.cpp,排除在 acecode_testable 外。
//
// 决策表(详见 spec/legacy-terminal-fallback/spec.md "Render mode 决策"):
//   alt_screen_mode == "always"  → AltScreen
//   alt_screen_mode == "never"   → TerminalOutput
//   alt_screen_mode == "auto" + WT_SESSION 命中 → TerminalOutput(short-circuit)
//   alt_screen_mode == "auto" + ConEmuPID 或 legacy conhost 命中 → AltScreen
//   alt_screen_mode == "auto" + 三个信号全无 → TerminalOutput(默认行为)

#include "config/config.hpp"
#include "utils/terminal_capability.hpp"

namespace acecode::tui {

enum class ScreenRenderMode {
    TerminalOutput, // 非 alt-screen,FTXUI 默认 — 历史行为
    AltScreen,      // \033[?1049h,适合 legacy 终端
};

inline ScreenRenderMode decide_render_mode(const TuiConfig& cfg,
                                            const TerminalCapabilities& caps) {
    // 显式覆盖优先于自动探测。
    if (cfg.alt_screen_mode == "always") return ScreenRenderMode::AltScreen;
    if (cfg.alt_screen_mode == "never")  return ScreenRenderMode::TerminalOutput;

    // auto 路径:WT_SESSION 命中即认定为现代终端,直接 short-circuit。
    if (caps.is_windows_terminal) return ScreenRenderMode::TerminalOutput;

    // 否则 ConEmu 或 legacy conhost 命中 → fallback 到 alt-screen。
    if (caps.is_conemu || caps.is_legacy_conhost) return ScreenRenderMode::AltScreen;

    // 三个信号全无 → 默认行为。
    return ScreenRenderMode::TerminalOutput;
}

} // namespace acecode::tui

// 工厂在 render_mode.cpp 实现,声明放在外层命名空间避免 picker_scroll
// 这种 header-only 用例误用 FTXUI。需要构造 ScreenInteractive 的调用站点
// 直接 include "tui/render_mode_factory.hpp"。
