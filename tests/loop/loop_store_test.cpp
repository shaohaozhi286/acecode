#include <gtest/gtest.h>

#include "loop/loop_store.hpp"

#include <filesystem>
#include <random>

namespace fs = std::filesystem;
using namespace acecode::loop;

namespace {

fs::path temp_database(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_loop_store_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir / "scheduled-loops.sqlite3";
}

LoopDefinition once_loop(const std::string& name,
                         std::int64_t when,
                         const std::string& workspace = {}) {
    LoopDefinition value;
    value.name = name;
    value.prompt = "Inspect and finish the requested code work.";
    value.model_name = "model-a";
    value.permission_mode = "yolo";
    if (!workspace.empty()) {
        value.workspace_hash = workspace;
        value.workspace_cwd = "C:/workspace/" + workspace;
    }
    value.schedule.kind = ScheduleKind::Once;
    value.schedule.once_at_ms = when;
    return value;
}

LoopDefinition daily_loop(const std::string& name,
                          int hour,
                          const std::string& workspace) {
    auto value = once_loop(name, 1);
    value.workspace_hash = workspace;
    value.workspace_cwd = "C:/workspace/" + workspace;
    value.schedule.kind = ScheduleKind::Period;
    value.schedule.period = PeriodKind::Daily;
    value.schedule.hour = hour;
    value.schedule.minute = 0;
    value.schedule.timezone_offset_minutes = 0;
    return value;
}

} // namespace

TEST(LoopStore, InitializesCrudAndEnableState) {
    const auto path = temp_database("crud");
    LoopStore store(path);
    StoreError error;
    ASSERT_TRUE(store.initialize(&error)) << error.message;
    EXPECT_TRUE(fs::exists(path));

    auto created = store.create_loop(once_loop("one", 10'000), 1'000, &error);
    ASSERT_TRUE(created.has_value()) << error.message;
    EXPECT_FALSE(created->id.empty());
    EXPECT_EQ(created->next_run_at_ms, 10'000);

    auto listed = store.list_loops(&error);
    ASSERT_EQ(listed.size(), 1u) << error.message;
    EXPECT_EQ(listed[0].name, "one");

    auto edit = *created;
    edit.name = "renamed";
    edit.prompt = "updated prompt";
    auto updated = store.update_loop(created->id, edit, 2'000, &error);
    ASSERT_TRUE(updated.has_value()) << error.message;
    EXPECT_EQ(updated->name, "renamed");
    EXPECT_EQ(updated->created_at_ms, created->created_at_ms);

    auto disabled = store.set_loop_enabled(created->id, false, 3'000, &error);
    ASSERT_TRUE(disabled.has_value()) << error.message;
    EXPECT_FALSE(disabled->enabled);
    EXPECT_FALSE(disabled->next_run_at_ms.has_value());

    ASSERT_TRUE(store.delete_loop(created->id, &error)) << error.message;
    EXPECT_TRUE(store.list_loops(&error).empty());
}

TEST(LoopStore, ConflictUpdateRollsBack) {
    LoopStore store(temp_database("conflict"));
    ASSERT_TRUE(store.initialize());
    StoreError error;
    auto first = store.create_loop(daily_loop("first", 9, "same"), 0, &error);
    auto second = store.create_loop(daily_loop("second", 10, "same"), 0, &error);
    ASSERT_TRUE(first.has_value()) << error.message;
    ASSERT_TRUE(second.has_value()) << error.message;

    auto conflicting = *second;
    conflicting.schedule.hour = 9;
    EXPECT_FALSE(store.update_loop(second->id, conflicting, 0, &error).has_value());
    EXPECT_EQ(error.code, "SCHEDULE_CONFLICT");
    ASSERT_TRUE(error.conflict.has_value());
    EXPECT_EQ(error.conflict->loop_id, first->id);

    error = {};
    auto unchanged = store.get_loop(second->id, &error);
    ASSERT_TRUE(unchanged.has_value()) << error.message;
    EXPECT_EQ(unchanged->schedule.hour, 10);
}

TEST(LoopStore, TwoConnectionsClaimOccurrenceOnlyOnce) {
    const auto path = temp_database("claim");
    LoopStore first(path);
    LoopStore second(path);
    ASSERT_TRUE(first.initialize());
    ASSERT_TRUE(second.initialize());
    auto loop = first.create_loop(once_loop("once", 10'000), 1'000);
    ASSERT_TRUE(loop.has_value());

    StoreError error;
    auto claim_a = first.claim_due(10'000, "owner-a", &error);
    ASSERT_EQ(claim_a.disposition, ClaimDisposition::Claimed) << error.message;
    auto claim_b = second.claim_due(10'000, "owner-b", &error);
    EXPECT_EQ(claim_b.disposition, ClaimDisposition::None);

    auto runs = first.list_runs(loop->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u) << error.message;
    EXPECT_EQ(runs[0].owner_id, "owner-a");
    EXPECT_EQ(runs[0].status, RunStatus::Running);
}

TEST(LoopStore, WorkspaceBusyOccurrenceIsMissed) {
    LoopStore store(temp_database("busy"));
    ASSERT_TRUE(store.initialize());
    auto first = store.create_loop(once_loop("first", 10'000, "workspace"), 0);
    auto second = store.create_loop(once_loop("second", 70'000, "workspace"), 0);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(store.claim_due(10'000, "owner").disposition, ClaimDisposition::Claimed);
    auto busy = store.claim_due(70'000, "owner");
    ASSERT_EQ(busy.disposition, ClaimDisposition::MissedWorkspaceBusy);
    ASSERT_TRUE(busy.run.has_value());
    EXPECT_EQ(busy.run->reason, "workspace_busy");
    EXPECT_EQ(busy.run->status, RunStatus::Missed);
}

TEST(LoopStore, AggregatesOfflineMissedAndAdvances) {
    LoopStore store(temp_database("offline"));
    ASSERT_TRUE(store.initialize());
    LoopDefinition value = once_loop("interval", 1);
    value.schedule.kind = ScheduleKind::Interval;
    value.schedule.interval_value = 30;
    value.schedule.interval_unit = IntervalUnit::Minutes;
    value.schedule.anchor_ms = 1'000;
    auto created = store.create_loop(value, 0);
    ASSERT_TRUE(created.has_value());

    const auto now = 1'000 + 95 * kLoopMinuteMs;
    StoreError error;
    ASSERT_TRUE(store.record_offline_missed(now, "owner", &error)) << error.message;
    auto runs = store.list_runs(created->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u) << error.message;
    EXPECT_EQ(runs[0].status, RunStatus::Missed);
    EXPECT_EQ(runs[0].reason, "daemon_offline");
    EXPECT_EQ(runs[0].missed_count, 4);
    auto updated = store.get_loop(created->id, &error);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->next_run_at_ms, 1'000 + 120 * kLoopMinuteMs);
}

TEST(LoopStore, UpdatesRunLifecycleAndInterruptsOwner) {
    LoopStore store(temp_database("lifecycle"));
    ASSERT_TRUE(store.initialize());
    auto loop = store.create_loop(once_loop("once", 10'000), 0);
    ASSERT_TRUE(loop.has_value());
    auto claim = store.claim_due(10'000, "owner-a");
    ASSERT_TRUE(claim.run.has_value());

    StoreError error;
    ASSERT_TRUE(store.update_run_state(
        claim.run->id, RunStatus::WaitingUser, 11'000, {}, "session-1", "wt", "branch", &error))
        << error.message;
    auto runs = store.list_runs(loop->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].status, RunStatus::WaitingUser);
    EXPECT_EQ(runs[0].session_id, "session-1");
    EXPECT_FALSE(runs[0].finished_at_ms.has_value());

    ASSERT_TRUE(store.interrupt_owner_runs("owner-a", 12'000, &error)) << error.message;
    runs = store.list_runs(loop->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].status, RunStatus::Failed);
    EXPECT_EQ(runs[0].reason, "daemon_interrupted");
    EXPECT_EQ(runs[0].finished_at_ms, 12'000);
}

TEST(LoopStore, NoWorkspaceLoopsMayShareTheSameSchedule) {
    LoopStore store(temp_database("no_workspace"));
    ASSERT_TRUE(store.initialize());
    StoreError error;
    ASSERT_TRUE(store.create_loop(daily_loop("with", 9, "workspace"), 0, &error).has_value())
        << error.message;
    auto first = daily_loop("first", 9, "");
    first.workspace_hash.clear();
    first.workspace_cwd.clear();
    auto second = first;
    second.name = "second";
    ASSERT_TRUE(store.create_loop(first, 0, &error).has_value()) << error.message;
    ASSERT_TRUE(store.create_loop(second, 0, &error).has_value()) << error.message;
}
