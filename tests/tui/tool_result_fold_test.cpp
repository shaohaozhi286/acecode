#include "tui/tool_result_fold.hpp"

#include <gtest/gtest.h>

#include <ftxui/screen/string.hpp>

#include <string>

namespace acecode::tui {

TEST(ToolResultFoldTest, KeepsShortSingleLineUnchanged) {
    const auto preview = fold_tool_result_preview("short result", 40, 3);

    ASSERT_EQ(preview.lines.size(), 1u);
    EXPECT_EQ(preview.lines[0], "short result");
    EXPECT_FALSE(preview.folded);
}

TEST(ToolResultFoldTest, FoldsOrdinaryHardLinesAtRowLimit) {
    const auto preview = fold_tool_result_preview(
        "first\nsecond\nthird\nfourth", 80, 3);

    ASSERT_EQ(preview.lines.size(), 3u);
    EXPECT_EQ(preview.lines[0], "first");
    EXPECT_EQ(preview.lines[1], "second");
    EXPECT_EQ(preview.lines[2], "third");
    EXPECT_TRUE(preview.folded);
}

TEST(ToolResultFoldTest, FoldsLongJsonWithEscapedNewlines) {
    std::string content = "[\"\\nCursor Position: (1191, 1084)\\n";
    for (int i = 0; i < 20; ++i) {
        content += "window name status width height handle\\n";
    }
    content += "\"]";
    ASSERT_EQ(content.find('\n'), std::string::npos);

    const auto preview = fold_tool_result_preview(content, 24, 3);

    ASSERT_EQ(preview.lines.size(), 3u);
    EXPECT_TRUE(preview.folded);
    for (const auto& line : preview.lines) {
        EXPECT_LE(ftxui::string_width(line), 24);
    }
    EXPECT_NE(preview.lines[0].find("\\n"), std::string::npos);
}

TEST(ToolResultFoldTest, PreservesUtf8BoundariesForWideGlyphs) {
    const std::string wide = "\xE7\x95\x8C"; // U+754C, terminal width 2.
    std::string content;
    for (int i = 0; i < 8; ++i) content += wide;

    const auto preview = fold_tool_result_preview(content, 6, 2);

    ASSERT_EQ(preview.lines.size(), 2u);
    EXPECT_TRUE(preview.folded);
    for (const auto& line : preview.lines) {
        EXPECT_LE(ftxui::string_width(line), 6);
        EXPECT_EQ(line.size() % wide.size(), 0u);
    }
}

} // namespace acecode::tui
