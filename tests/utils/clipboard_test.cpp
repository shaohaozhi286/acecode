#include <gtest/gtest.h>

#include "utils/clipboard.hpp"

#include <string>

using acecode::ClipboardTextReadResult;
using acecode::ClipboardTextWriteResult;
using acecode::linux_clipboard_text_commands;
using acecode::linux_clipboard_write_commands;
using acecode::read_system_clipboard_text_from_commands;
using acecode::write_system_clipboard_text_from_commands;

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

TEST(ClipboardTest, LinuxWriteCommandCandidatesPreferWaylandThenX11) {
    auto commands = linux_clipboard_write_commands(true, true);
    ASSERT_EQ(commands.size(), 3u);
    EXPECT_NE(commands[0].find("wl-copy"), std::string::npos);
    EXPECT_NE(commands[1].find("xclip"), std::string::npos);
    EXPECT_NE(commands[1].find("clipboard"), std::string::npos);
    EXPECT_NE(commands[2].find("xsel"), std::string::npos);
    EXPECT_NE(commands[2].find("clipboard"), std::string::npos);
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

TEST(ClipboardTest, LinuxWriteCommandCandidatesUseX11ToolsWhenOnlyDisplayExists) {
    auto commands = linux_clipboard_write_commands(false, true);
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_NE(commands[0].find("xclip"), std::string::npos);
    EXPECT_NE(commands[0].find("clipboard"), std::string::npos);
    EXPECT_NE(commands[1].find("xsel"), std::string::npos);
    EXPECT_NE(commands[1].find("clipboard"), std::string::npos);
}

TEST(ClipboardTest, LinuxCommandCandidatesEmptyWithoutDisplay) {
    auto commands = linux_clipboard_text_commands(false, false);
    EXPECT_TRUE(commands.empty());
}

TEST(ClipboardTest, LinuxWriteCommandCandidatesEmptyWithoutDisplay) {
    auto commands = linux_clipboard_write_commands(false, false);
    EXPECT_TRUE(commands.empty());
}

TEST(ClipboardTest, EmptyCommandListIsUnavailable) {
    auto result = read_system_clipboard_text_from_commands({}, 1024);
    EXPECT_EQ(result.status, ClipboardTextReadResult::Status::Unavailable);
    EXPECT_TRUE(result.text.empty());
}

TEST(ClipboardTest, EmptyWriteCommandListIsUnavailable) {
    auto result = write_system_clipboard_text_from_commands({}, "hello", 1024);
    EXPECT_EQ(result.status, ClipboardTextWriteResult::Status::Unavailable);
}

TEST(ClipboardTest, WriteCommandRejectsTooLargeInput) {
    auto result = write_system_clipboard_text_from_commands(
        {"cat >/dev/null"}, "hello", 4);
    EXPECT_EQ(result.status, ClipboardTextWriteResult::Status::TooLarge);
}

TEST(ClipboardTest, WriteCommandReportsSuccess) {
#ifdef _WIN32
    const char* command = "cmd /C more > NUL";
#else
    const char* command = "cat >/dev/null";
#endif
    auto result = write_system_clipboard_text_from_commands(
        {command}, "hello", 1024);
    EXPECT_EQ(result.status, ClipboardTextWriteResult::Status::Success);
}
