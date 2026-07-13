#include <gtest/gtest.h>

#include <ftxui/screen/color.hpp>

#include "tui/theme_palette.hpp"
#include "tui/tool_row_presentation.hpp"

namespace acecode::tui {

TEST(ToolRowPresentation, ArgumentsAreVisibleOnlyInGlobalVerboseMode) {
    EXPECT_FALSE(tool_call_arguments_visible(false));
    EXPECT_TRUE(tool_call_arguments_visible(true));
}

TEST(ToolRowPresentation, DarkThemeUsesBrightNameAndNormalGrayText) {
    const auto palette = make_dark_palette();

    EXPECT_EQ(tool_call_name_color(palette), ftxui::Color::White);
    EXPECT_EQ(tool_call_name_color(palette), palette.ui.text_primary);
    EXPECT_EQ(tool_call_argument_color(palette), palette.ui.text_muted);
    EXPECT_EQ(tool_result_text_color(palette), palette.ui.text_muted);
}

TEST(ToolRowPresentation, LightThemeKeepsExistingNameColor) {
    const auto palette = make_light_palette();

    EXPECT_EQ(tool_call_name_color(palette), palette.syntax.preproc);
    EXPECT_EQ(tool_call_argument_color(palette), palette.ui.text_muted);
    EXPECT_EQ(tool_result_text_color(palette), palette.ui.text_muted);
}

} // namespace acecode::tui
