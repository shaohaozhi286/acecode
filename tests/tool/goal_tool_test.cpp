#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/goal_tool.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>

namespace fs = std::filesystem;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_goal_tool_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

acecode::ToolContext context_for(acecode::SessionManager& sm) {
    acecode::ToolContext ctx;
    ctx.session_manager = &sm;
    return ctx;
}

} // namespace

TEST(GoalTool, GetGoalReturnsNullWhenMissing) {
    auto cwd = temp_cwd("get_null");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test", "model", "sid-goal-null");

    auto tool = acecode::create_get_goal_tool();
    auto result = tool.execute("{}", context_for(sm));
    ASSERT_TRUE(result.success) << result.output;
    auto json = nlohmann::json::parse(result.output);
    EXPECT_TRUE(json["goal"].is_null());
}

TEST(GoalTool, CreateGoalCreatesOnlyWhenMissing) {
    auto cwd = temp_cwd("create");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test", "model", "sid-goal-create");

    int update_events = 0;
    auto ctx = context_for(sm);
    ctx.emit_goal_updated = [&](const nlohmann::json&) { update_events++; };

    auto create = acecode::create_create_goal_tool();
    auto result = create.execute(
        R"({"objective":"finish tests","token_budget":1234})", ctx);
    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(update_events, 1);
    auto json = nlohmann::json::parse(result.output);
    ASSERT_FALSE(json["goal"].is_null());
    EXPECT_EQ(json["goal"]["objective"], "finish tests");
    EXPECT_EQ(json["goal"]["token_budget"], 1234);

    auto rejected = create.execute(R"({"objective":"replace it"})", ctx);
    EXPECT_FALSE(rejected.success);
    EXPECT_NE(rejected.output.find("already exists"), std::string::npos);
}

TEST(GoalTool, UpdateGoalAllowsCompleteAndBlocked) {
    auto cwd = temp_cwd("update");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test", "model", "sid-goal-update");
    ASSERT_FALSE(sm.ensure_active_session_id().empty());
    ASSERT_TRUE(sm.goal_store()->replace_thread_goal(
        sm.current_session_id(), "finish update", std::nullopt, acecode::ThreadGoalStatus::Active));

    int account_calls = 0;
    int update_events = 0;
    auto ctx = context_for(sm);
    ctx.account_goal_usage = [&] { account_calls++; };
    ctx.emit_goal_updated = [&](const nlohmann::json&) { update_events++; };

    auto update = acecode::create_update_goal_tool();
    auto rejected = update.execute(R"({"status":"paused"})", ctx);
    EXPECT_FALSE(rejected.success);

    auto blocked = update.execute(R"({"status":"blocked"})", ctx);
    ASSERT_TRUE(blocked.success) << blocked.output;
    EXPECT_EQ(account_calls, 1);
    EXPECT_EQ(update_events, 1);
    auto blocked_goal = sm.goal_store()->get_thread_goal(sm.current_session_id());
    ASSERT_TRUE(blocked_goal.has_value());
    EXPECT_EQ(blocked_goal->status, acecode::ThreadGoalStatus::Blocked);

    ASSERT_TRUE(sm.goal_store()->replace_thread_goal(
        sm.current_session_id(), "finish update", std::nullopt, acecode::ThreadGoalStatus::Active));

    auto done = update.execute(R"({"status":"complete"})", ctx);
    ASSERT_TRUE(done.success) << done.output;
    EXPECT_EQ(account_calls, 2);
    EXPECT_EQ(update_events, 2);
    auto goal = sm.goal_store()->get_thread_goal(sm.current_session_id());
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Complete);
}
