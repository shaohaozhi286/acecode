#include <gtest/gtest.h>

#include "utils/clipboard.hpp"

#include <string>

using acecode::ClipboardTextReadResult;
using acecode::linux_clipboard_text_commands;
using acecode::read_system_clipboard_text_from_commands;

TEST(ClipboardTest, LinuxCommandCandidatesPreferWaylandThenX11) {
    auto commands = linux_clipboard_text_commands(true, true);
    ASSERT_EQ(commands.size(), 5u);
    EXPECT_NE(commands[0].find("wl-paste"), std::string::npos);
    EXPECT_NE(commands[1].find("xclip"), std::string::npos);
    EXPECT_NE(commands[1].find("clipboard"), std::string::npos);
    EXPECT_NE(commands[2].find("xsel"), std::string::npos);
    EXPECT_NE(commands[2].find("clipboard"), std::string::npos);
    EXPECT_NE(commands[3].find("xclip"), std::string::npos);
    EXPECT_NE(commands[3].find("primary"), std::string::npos);
    EXPECT_NE(commands[4].find("xsel"), std::string::npos);
    EXPECT_NE(commands[4].find("primary"), std::string::npos);
}

TEST(ClipboardTest, LinuxCommandCandidatesUseX11ToolsWhenOnlyDisplayExists) {
    auto commands = linux_clipboard_text_commands(false, true);
    ASSERT_EQ(commands.size(), 4u);
    EXPECT_NE(commands[0].find("xclip"), std::string::npos);
    EXPECT_NE(commands[0].find("clipboard"), std::string::npos);
    EXPECT_NE(commands[1].find("xsel"), std::string::npos);
    EXPECT_NE(commands[1].find("clipboard"), std::string::npos);
    EXPECT_NE(commands[2].find("xclip"), std::string::npos);
    EXPECT_NE(commands[2].find("primary"), std::string::npos);
    EXPECT_NE(commands[3].find("xsel"), std::string::npos);
    EXPECT_NE(commands[3].find("primary"), std::string::npos);
}

TEST(ClipboardTest, LinuxCommandCandidatesEmptyWithoutDisplay) {
    auto commands = linux_clipboard_text_commands(false, false);
    EXPECT_TRUE(commands.empty());
}

TEST(ClipboardTest, EmptyCommandListIsUnavailable) {
    auto result = read_system_clipboard_text_from_commands({}, 1024);
    EXPECT_EQ(result.status, ClipboardTextReadResult::Status::Unavailable);
    EXPECT_TRUE(result.text.empty());
}
