#include <gtest/gtest.h>

#include "tui_state.hpp"

using acecode::is_shell_mode_trigger_character;

TEST(ShellInputMode, TriggerCharacterAcceptsAsciiExclamation) {
    EXPECT_TRUE(is_shell_mode_trigger_character("!"));
}

TEST(ShellInputMode, TriggerCharacterAcceptsFullWidthExclamation) {
    EXPECT_TRUE(is_shell_mode_trigger_character("\xEF\xBC\x81"));
}

TEST(ShellInputMode, TriggerCharacterRejectsOtherText) {
    EXPECT_FALSE(is_shell_mode_trigger_character(""));
    EXPECT_FALSE(is_shell_mode_trigger_character("!!"));
    EXPECT_FALSE(is_shell_mode_trigger_character("x"));
}
