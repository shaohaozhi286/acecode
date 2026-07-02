#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/plan_mode_tool.hpp"

#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_plan_mode_tool_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

} // namespace

TEST(PlanModeTool, EnterPlanModeUsesRuntimeCallback) {
    auto tool = acecode::create_enter_plan_mode_tool();
    bool called = false;

    acecode::ToolContext ctx;
    ctx.enter_plan_mode = [&] {
        called = true;
        return std::string{"C:/tmp/plan.md"};
    };

    auto result = tool.execute("{}", ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_TRUE(called);
    EXPECT_NE(result.output.find("Entered plan mode"), std::string::npos);
    EXPECT_NE(result.output.find("C:/tmp/plan.md"), std::string::npos);
    EXPECT_NE(result.output.find("DO NOT write or edit"), std::string::npos);
}

TEST(PlanModeTool, EnterPlanModeNoOpsInYoloMode) {
    auto tool = acecode::create_enter_plan_mode_tool();
    bool called = false;

    acecode::ToolContext ctx;
    ctx.current_permission_mode = [] { return std::string{"yolo"}; };
    ctx.enter_plan_mode = [&] {
        called = true;
        return std::string{"C:/tmp/plan.md"};
    };

    auto result = tool.execute("{}", ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_FALSE(called);
    EXPECT_EQ(result.output, "OK");
}

TEST(PlanModeTool, ExitPlanModeNoOpsInYoloMode) {
    auto tool = acecode::create_exit_plan_mode_tool();
    bool called = false;

    acecode::ToolContext ctx;
    ctx.current_permission_mode = [] { return std::string{"yolo"}; };
    ctx.exit_plan_mode = [&] {
        called = true;
        return std::string{"default"};
    };

    auto result = tool.execute("{}", ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_FALSE(called);
    EXPECT_EQ(result.output, "OK");
}

TEST(PlanModeTool, ExitPlanModeFailsOutsidePlanMode) {
    auto tool = acecode::create_exit_plan_mode_tool();

    acecode::ToolContext ctx;
    ctx.current_permission_mode = [] { return std::string{"default"}; };

    auto result = tool.execute("{}", ctx);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("not in plan mode"), std::string::npos);
}

TEST(PlanModeTool, ExitPlanModeReadsPlanAndRestoresPreviousMode) {
    auto cwd = temp_cwd("exit");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "sid-plan-tool");
    ASSERT_FALSE(sm.ensure_plan_file_path().empty());

    std::string error;
    ASSERT_TRUE(sm.write_plan_file("1. Inspect\n2. Implement\n", &error)) << error;

    std::string mode = "plan";
    acecode::ToolContext ctx;
    ctx.session_manager = &sm;
    ctx.current_permission_mode = [&] { return mode; };
    ctx.exit_plan_mode = [&] {
        mode = "accept-edits";
        return mode;
    };

    auto tool = acecode::create_exit_plan_mode_tool();
    auto result = tool.execute("{}", ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(mode, "accept-edits");
    EXPECT_NE(result.output.find("User has approved your plan"), std::string::npos);
    EXPECT_NE(result.output.find("Restored permission mode: accept-edits"), std::string::npos);
    EXPECT_NE(result.output.find("1. Inspect"), std::string::npos);
    EXPECT_NE(result.output.find("2. Implement"), std::string::npos);

    fs::remove_all(acecode::SessionStorage::get_project_dir(cwd.string()));
    fs::remove_all(cwd);
}
