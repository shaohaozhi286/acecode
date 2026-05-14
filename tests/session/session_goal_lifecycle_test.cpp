#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"

#include <filesystem>
#include <optional>
#include <random>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_session_goal_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

acecode::ChatMessage msg(std::string role, std::string content) {
    acecode::ChatMessage m;
    m.role = std::move(role);
    m.content = std::move(content);
    return m;
}

} // namespace

TEST(SessionGoalLifecycle, ResumeLoadsPersistedGoalFromProjectSqlite) {
    auto cwd = temp_cwd("resume");
    const std::string sid = "sid-goal-resume";

    acecode::SessionManager writer;
    writer.start_session(cwd.string(), "stub", "model", sid);
    writer.on_message(msg("user", "hello"));
    ASSERT_TRUE(writer.goal_store()->replace_thread_goal(
        sid, "persist this goal", 5000, acecode::ThreadGoalStatus::Active));

    acecode::SessionManager reader;
    reader.start_session(cwd.string(), "stub", "model");
    auto messages = reader.resume_session(sid);
    ASSERT_FALSE(messages.empty());
    auto* store = reader.existing_goal_store();
    ASSERT_NE(store, nullptr);
    auto goal = store->get_thread_goal(sid);
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->objective, "persist this goal");
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Active);
    EXPECT_EQ(goal->token_budget, 5000);

    fs::remove_all(cwd);
    fs::remove_all(acecode::SessionStorage::get_project_dir(cwd.string()));
}

TEST(SessionGoalLifecycle, ForkCopiesGoalWithNewIdAndResetCounters) {
    auto cwd = temp_cwd("fork");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "stub", "model", "sid-goal-fork-source");
    sm.on_message(msg("user", "keep"));

    const std::string source_sid = sm.current_session_id();
    ASSERT_TRUE(sm.goal_store()->replace_thread_goal(
        source_sid, "forkable goal", 5000, acecode::ThreadGoalStatus::Active));
    auto source_goal = sm.goal_store()->get_thread_goal(source_sid);
    ASSERT_TRUE(source_goal.has_value());
    auto accounted = sm.goal_store()->account_thread_goal_usage(
        source_sid, source_goal->goal_id, 1200, 33, false);
    ASSERT_TRUE(accounted.goal.has_value());
    EXPECT_EQ(accounted.goal->tokens_used, 1200);

    const std::vector<acecode::ChatMessage> retained{msg("user", "keep")};
    const std::string fork_sid = sm.fork_active_session(retained);
    ASSERT_FALSE(fork_sid.empty());

    auto fork_goal = sm.goal_store()->get_thread_goal(fork_sid);
    ASSERT_TRUE(fork_goal.has_value());
    EXPECT_EQ(fork_goal->objective, "forkable goal");
    EXPECT_EQ(fork_goal->status, acecode::ThreadGoalStatus::Active);
    EXPECT_EQ(fork_goal->token_budget, 5000);
    EXPECT_EQ(fork_goal->tokens_used, 0);
    EXPECT_EQ(fork_goal->time_used_seconds, 0);
    EXPECT_NE(fork_goal->goal_id, source_goal->goal_id);

    fs::remove_all(cwd);
    fs::remove_all(acecode::SessionStorage::get_project_dir(cwd.string()));
}

TEST(SessionGoalLifecycle, CompactRewriteLeavesGoalRowIntact) {
    auto cwd = temp_cwd("compact");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "stub", "model", "sid-goal-compact");
    sm.on_message(msg("user", "before compact"));
    const std::string sid = sm.current_session_id();
    ASSERT_TRUE(sm.goal_store()->replace_thread_goal(
        sid, "survive compact", std::nullopt, acecode::ThreadGoalStatus::Paused));

    ASSERT_TRUE(sm.replace_active_messages({msg("system", "[Conversation summary]")}));
    auto goal = sm.goal_store()->get_thread_goal(sid);
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->objective, "survive compact");
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Paused);

    fs::remove_all(cwd);
    fs::remove_all(acecode::SessionStorage::get_project_dir(cwd.string()));
}
