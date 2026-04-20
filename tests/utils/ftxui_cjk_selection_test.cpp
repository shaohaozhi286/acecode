// 覆盖 FTXUI overlay port 中 `src/ftxui/dom/text.cpp` 的 CJK 选区修复。
// Utf8ToGlyphs 对全角字符会产出 (字符, "") 两项，分别占据终端相邻两列；
// 原实现的 Text::Select 只按第一格判定，导致用户从续列起拖或单击续列时漏字。
// 这里用 ftxui 公开 API 直接构造 Selection + Render，断言复制到的字符串。

#include <gtest/gtest.h>

#include "ftxui/dom/elements.hpp"   // text
#include "ftxui/dom/node.hpp"       // Render
#include "ftxui/dom/selection.hpp"  // Selection
#include "ftxui/screen/screen.hpp"  // Screen

namespace {

// 场景：在 "你好世界" (四个全角字符，占据 col 0..7) 上，从 col=1（你的续列）
// 拖到 col=3（好的续列）。修复前返回 "好"，修复后应返回 "你好"。
TEST(FtxuiCjkSelection, StartOnTrailingCell) {
    auto element = ftxui::text("你好世界");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(16),
                                        ftxui::Dimension::Fixed(1));
    ftxui::Selection selection(1, 0, 3, 0);
    ftxui::Render(screen, element.get(), selection);
    EXPECT_EQ(selection.GetParts(), "你好");
}

// 场景：单击正好落在全角字符 "你" 的续列 (col=1)。修复前返回空串，
// 修复后应返回 "你"。保证点到续列不会丢字。
TEST(FtxuiCjkSelection, SingleCellOnTrailingCell) {
    auto element = ftxui::text("你好世界");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(16),
                                        ftxui::Dimension::Fixed(1));
    ftxui::Selection selection(1, 0, 1, 0);
    ftxui::Render(screen, element.get(), selection);
    EXPECT_EQ(selection.GetParts(), "你");
}

// 场景：ASCII 与 CJK 混排 "Hi你好"（H=0, i=1, 你=2..3, 好=4..5），
// 从 col=3（你的续列）拖到 col=5（好的续列）。修复前返回 "好"，
// 修复后应返回 "你好"。验证混排场景的 ASCII 选区行为无回归。
TEST(FtxuiCjkSelection, MixedAsciiCjk) {
    auto element = ftxui::text("Hi你好");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(10),
                                        ftxui::Dimension::Fixed(1));
    ftxui::Selection selection(3, 0, 5, 0);
    ftxui::Render(screen, element.get(), selection);
    EXPECT_EQ(selection.GetParts(), "你好");
}

// 场景：纯 ASCII 选区（原本就能正确工作），确保 fix 对 narrow 字符没有回归。
// "Hello World" 上从 col=0 到 col=4 应返回 "Hello"。
TEST(FtxuiCjkSelection, AsciiOnlyNoRegression) {
    auto element = ftxui::text("Hello World");
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(20),
                                        ftxui::Dimension::Fixed(1));
    ftxui::Selection selection(0, 0, 4, 0);
    ftxui::Render(screen, element.get(), selection);
    EXPECT_EQ(selection.GetParts(), "Hello");
}

}  // namespace
