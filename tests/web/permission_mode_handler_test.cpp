// Covers pure helpers for /api/sessions/:id/permissions.

#include <gtest/gtest.h>

#include "web/handlers/permission_mode_handler.hpp"

using acecode::PermissionMode;
using acecode::web::parse_permission_mode_name;
using acecode::web::permission_mode_to_json;

TEST(PermissionModeHandler, ParsesCanonicalAndLegacyNames) {
    EXPECT_EQ(parse_permission_mode_name("default"), PermissionMode::Default);
    EXPECT_EQ(parse_permission_mode_name("accept-edits"), PermissionMode::AcceptEdits);
    EXPECT_EQ(parse_permission_mode_name("acceptEdits"), PermissionMode::AcceptEdits);
    EXPECT_EQ(parse_permission_mode_name("plan"), PermissionMode::Plan);
    EXPECT_EQ(parse_permission_mode_name("yolo"), PermissionMode::Yolo);
    EXPECT_FALSE(parse_permission_mode_name("ask").has_value());
    EXPECT_FALSE(parse_permission_mode_name("").has_value());
}

TEST(PermissionModeHandler, SerializesCanonicalModeName) {
    auto j = permission_mode_to_json(PermissionMode::AcceptEdits);
    EXPECT_EQ(j["mode"], "accept-edits");
    EXPECT_TRUE(j.contains("description"));

    auto plan = permission_mode_to_json(PermissionMode::Plan);
    EXPECT_EQ(plan["mode"], "plan");
    EXPECT_TRUE(plan["description"].get<std::string>().find("Plan") != std::string::npos);
}
