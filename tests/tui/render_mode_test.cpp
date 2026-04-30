// 覆盖 src/tui/render_mode.hpp 中 decide_render_mode() 的纯函数行为。
// 不涉及 FTXUI(make_screen_interactive 在 .cpp 里,排除在 acecode_testable
// 之外不单测,跟 src/tui 其它 FTXUI-bound 文件保持一致)。
//
// 决策表参见 spec/legacy-terminal-fallback/spec.md "Render mode 决策":
//   alt_screen_mode == "always"  → AltScreen
//   alt_screen_mode == "never"   → TerminalOutput
//   auto + WT_SESSION 命中       → TerminalOutput(short-circuit)
//   auto + ConEmu 或 legacy     → AltScreen
//   auto + 全空                 → TerminalOutput

#include <gtest/gtest.h>

#include "tui/render_mode.hpp"

using namespace acecode;
using namespace acecode::tui;

namespace {

TerminalCapabilities make_caps(bool conemu, bool wt, bool legacy) {
    TerminalCapabilities c;
    c.is_conemu = conemu;
    c.is_windows_terminal = wt;
    c.is_legacy_conhost = legacy;
    if (conemu) c.source_label = "Cmder/ConEmu";
    else if (legacy) c.source_label = "legacy Windows console";
    return c;
}

TuiConfig with_mode(const std::string& m) {
    TuiConfig t;
    t.alt_screen_mode = m;
    return t;
}

} // namespace

// 场景:always 模式无视所有探测信号
TEST(RenderModeDecide, AlwaysIgnoresAllCaps) {
    EXPECT_EQ(decide_render_mode(with_mode("always"),
                                  make_caps(false, false, false)),
              ScreenRenderMode::AltScreen);
    EXPECT_EQ(decide_render_mode(with_mode("always"),
                                  make_caps(false, true, false)), // 即便 Windows Terminal
              ScreenRenderMode::AltScreen);
}

// 场景:never 模式无视所有探测信号
TEST(RenderModeDecide, NeverIgnoresAllCaps) {
    EXPECT_EQ(decide_render_mode(with_mode("never"),
                                  make_caps(false, false, false)),
              ScreenRenderMode::TerminalOutput);
    EXPECT_EQ(decide_render_mode(with_mode("never"),
                                  make_caps(true, false, true)), // 即便 ConEmu + legacy
              ScreenRenderMode::TerminalOutput);
}

// 场景:auto + Windows Terminal 命中 → TerminalOutput(short-circuit)
TEST(RenderModeDecide, AutoWindowsTerminalShortCircuits) {
    EXPECT_EQ(decide_render_mode(with_mode("auto"),
                                  make_caps(false, true, false)),
              ScreenRenderMode::TerminalOutput);
}

// 场景:auto + WT 同时命中 ConEmu(理论不可能,但要决策稳定)
// → 仍 short-circuit 到 TerminalOutput,因为 WT 信号优先
TEST(RenderModeDecide, AutoWTBeatsConEmu) {
    EXPECT_EQ(decide_render_mode(with_mode("auto"),
                                  make_caps(true, true, false)),
              ScreenRenderMode::TerminalOutput);
}

// 场景:auto + 仅 ConEmu 命中 → AltScreen
TEST(RenderModeDecide, AutoConEmuTriggersAltScreen) {
    EXPECT_EQ(decide_render_mode(with_mode("auto"),
                                  make_caps(true, false, false)),
              ScreenRenderMode::AltScreen);
}

// 场景:auto + 仅 legacy conhost 命中 → AltScreen
TEST(RenderModeDecide, AutoLegacyConhostTriggersAltScreen) {
    EXPECT_EQ(decide_render_mode(with_mode("auto"),
                                  make_caps(false, false, true)),
              ScreenRenderMode::AltScreen);
}

// 场景:auto + ConEmu + legacy(老 Cmder 跑在老 Win10)→ AltScreen
TEST(RenderModeDecide, AutoConEmuPlusLegacyTriggersAltScreen) {
    EXPECT_EQ(decide_render_mode(with_mode("auto"),
                                  make_caps(true, false, true)),
              ScreenRenderMode::AltScreen);
}

// 场景:auto + 全空(macOS / Linux / 现代 Windows 无包装层)→ TerminalOutput
TEST(RenderModeDecide, AutoNoSignalsKeepsDefault) {
    EXPECT_EQ(decide_render_mode(with_mode("auto"),
                                  make_caps(false, false, false)),
              ScreenRenderMode::TerminalOutput);
}
