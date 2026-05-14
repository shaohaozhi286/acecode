#include <gtest/gtest.h>

#include "session/thread_goal_store.hpp"

#include <filesystem>
#include <optional>
#include <random>

namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_goal_store_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST(ThreadGoalStore, InitializesSqliteAndReadsMissingGoal) {
    auto dir = temp_dir("init");
    acecode::ThreadGoalStore store(dir);
    std::string error;

    ASSERT_TRUE(store.initialize(&error)) << error;
    EXPECT_TRUE(fs::exists(dir / "state.sqlite3"));
    EXPECT_FALSE(store.get_thread_goal("thread-a", &error).has_value());
    EXPECT_TRUE(error.empty());
}

TEST(ThreadGoalStore, ReplaceGetAndDeleteGoal) {
    auto dir = temp_dir("replace");
    acecode::ThreadGoalStore store(dir);
    ASSERT_TRUE(store.initialize());

    std::string error;
    ASSERT_TRUE(store.replace_thread_goal(
        "thread-a", "finish sqlite goals", 50000, acecode::ThreadGoalStatus::Active, &error)) << error;

    auto goal = store.get_thread_goal("thread-a", &error);
    ASSERT_TRUE(goal.has_value()) << error;
    EXPECT_EQ(goal->objective, "finish sqlite goals");
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Active);
    ASSERT_TRUE(goal->token_budget.has_value());
    EXPECT_EQ(*goal->token_budget, 50000);
    EXPECT_EQ(goal->tokens_used, 0);

    ASSERT_TRUE(store.delete_thread_goal("thread-a", &error)) << error;
    EXPECT_FALSE(store.get_thread_goal("thread-a", &error).has_value());
}

TEST(ThreadGoalStore, AccountingTransitionsToBudgetLimitedOnce) {
    auto dir = temp_dir("budget");
    acecode::ThreadGoalStore store(dir);
    ASSERT_TRUE(store.initialize());
    ASSERT_TRUE(store.replace_thread_goal(
        "thread-a", "stay within budget", 100, acecode::ThreadGoalStatus::Active));
    auto goal = store.get_thread_goal("thread-a");
    ASSERT_TRUE(goal.has_value());

    std::string error;
    auto first = store.account_thread_goal_usage(
        "thread-a", goal->goal_id, 40, 3, false, &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_TRUE(first.goal.has_value());
    EXPECT_TRUE(first.updated);
    EXPECT_FALSE(first.became_budget_limited);
    EXPECT_EQ(first.goal->tokens_used, 40);
    EXPECT_EQ(first.goal->time_used_seconds, 3);
    EXPECT_EQ(first.goal->status, acecode::ThreadGoalStatus::Active);

    auto second = store.account_thread_goal_usage(
        "thread-a", goal->goal_id, 60, 2, false, &error);
    ASSERT_TRUE(second.goal.has_value()) << error;
    EXPECT_TRUE(second.became_budget_limited);
    EXPECT_EQ(second.goal->tokens_used, 100);
    EXPECT_EQ(second.goal->time_used_seconds, 5);
    EXPECT_EQ(second.goal->status, acecode::ThreadGoalStatus::BudgetLimited);

    auto third = store.account_thread_goal_usage(
        "thread-a", goal->goal_id, 10, 1, false, &error);
    ASSERT_TRUE(third.goal.has_value()) << error;
    EXPECT_FALSE(third.updated);
    EXPECT_FALSE(third.became_budget_limited);
    EXPECT_EQ(third.goal->tokens_used, 100);
}

TEST(ThreadGoalStore, PauseAndCopyResetUsage) {
    auto dir = temp_dir("copy");
    acecode::ThreadGoalStore store(dir);
    ASSERT_TRUE(store.initialize());
    ASSERT_TRUE(store.replace_thread_goal(
        "source", "copy me", std::nullopt, acecode::ThreadGoalStatus::Active));
    auto source = store.get_thread_goal("source");
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(store.account_thread_goal_usage("source", source->goal_id, 25, 7, false).updated);
    ASSERT_TRUE(store.pause_active_thread_goal("source"));

    ASSERT_TRUE(store.copy_goal_reset_usage("source", "fork"));
    auto forked = store.get_thread_goal("fork");
    ASSERT_TRUE(forked.has_value());
    EXPECT_EQ(forked->objective, "copy me");
    EXPECT_EQ(forked->status, acecode::ThreadGoalStatus::Paused);
    EXPECT_EQ(forked->tokens_used, 0);
    EXPECT_EQ(forked->time_used_seconds, 0);
    EXPECT_NE(forked->goal_id, source->goal_id);
}

TEST(ThreadGoalStore, RejectsInvalidObjectiveAndBudget) {
    auto dir = temp_dir("invalid");
    acecode::ThreadGoalStore store(dir);
    ASSERT_TRUE(store.initialize());

    std::string error;
    EXPECT_FALSE(store.replace_thread_goal(
        "thread", "   ", std::nullopt, acecode::ThreadGoalStatus::Active, &error));
    EXPECT_NE(error.find("empty"), std::string::npos);

    error.clear();
    EXPECT_FALSE(store.replace_thread_goal(
        "thread", "valid", 0, acecode::ThreadGoalStatus::Active, &error));
    EXPECT_NE(error.find("positive"), std::string::npos);
}
