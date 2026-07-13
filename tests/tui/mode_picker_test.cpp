#include "tui/mode_picker.hpp"

#include <gtest/gtest.h>

using acecode::build_mode_picker_options;
using acecode::PermissionMode;

TEST(ModePicker, BuildsAllModesInCommandOrder) {
    const auto options = build_mode_picker_options(PermissionMode::Default);

    ASSERT_EQ(options.size(), 4u);
    EXPECT_EQ(options[0].mode, PermissionMode::Default);
    EXPECT_EQ(options[0].name, "default");
    EXPECT_EQ(options[1].mode, PermissionMode::AcceptEdits);
    EXPECT_EQ(options[1].name, "accept-edits");
    EXPECT_EQ(options[2].mode, PermissionMode::Plan);
    EXPECT_EQ(options[2].name, "plan");
    EXPECT_EQ(options[3].mode, PermissionMode::Yolo);
    EXPECT_EQ(options[3].name, "yolo");
}

TEST(ModePicker, UsesPermissionManagerDescriptions) {
    const auto options = build_mode_picker_options(PermissionMode::Default);

    for (const auto& option : options) {
        EXPECT_EQ(option.description,
                  acecode::PermissionManager::mode_description(option.mode));
        EXPECT_FALSE(option.description.empty());
    }
}

TEST(ModePicker, MarksOnlyCurrentMode) {
    const auto options = build_mode_picker_options(PermissionMode::Plan);

    ASSERT_EQ(options.size(), 4u);
    EXPECT_FALSE(options[0].is_current);
    EXPECT_FALSE(options[1].is_current);
    EXPECT_TRUE(options[2].is_current);
    EXPECT_FALSE(options[3].is_current);
}
