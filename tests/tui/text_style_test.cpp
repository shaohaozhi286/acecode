#include <gtest/gtest.h>

#include "tui/text_style.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

TEST(TuiTextStyle, DarkSecondaryTextIsReadableWithoutTerminalDimming) {
    acecode::tui::init_theme_palette("dark");

    ftxui::Screen screen(4, 1);
    ftxui::Render(
        screen,
        ftxui::text("hint") | acecode::tui::readable_secondary());

    const auto& cell = screen.CellAt(0, 0);
    EXPECT_EQ(cell.foreground_color,
              acecode::tui::theme().ui.text_secondary);
    EXPECT_NE(cell.foreground_color,
              acecode::tui::theme().ui.text_dim);
    EXPECT_FALSE(cell.dim);
}

TEST(TuiTextStyle, LightSecondaryTextUsesTheSameStableStyleContract) {
    acecode::tui::init_theme_palette("light");

    ftxui::Screen screen(4, 1);
    ftxui::Render(
        screen,
        ftxui::text("hint") | acecode::tui::readable_secondary());

    const auto& cell = screen.CellAt(0, 0);
    EXPECT_EQ(cell.foreground_color,
              acecode::tui::theme().ui.text_secondary);
    EXPECT_NE(cell.foreground_color,
              acecode::tui::theme().ui.text_dim);
    EXPECT_FALSE(cell.dim);

    acecode::tui::init_theme_palette("dark");
}
