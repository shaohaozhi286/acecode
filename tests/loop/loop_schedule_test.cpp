#include <gtest/gtest.h>

#include "loop/loop_schedule.hpp"

using namespace acecode::loop;

namespace {

LoopSchedule daily_at(int hour, int minute) {
    LoopSchedule value;
    value.kind = ScheduleKind::Period;
    value.period = PeriodKind::Daily;
    value.hour = hour;
    value.minute = minute;
    value.timezone_offset_minutes = 0;
    return value;
}

LoopDefinition loop_for_workspace(const std::string& id,
                                  const std::string& workspace,
                                  const LoopSchedule& schedule) {
    LoopDefinition value;
    value.id = id;
    value.name = id;
    value.prompt = "inspect";
    value.workspace_hash = workspace;
    value.workspace_cwd = "/workspace/" + workspace;
    value.model_name = "model";
    value.schedule = schedule;
    return value;
}

} // namespace

TEST(LoopSchedule, CompilesPeriodIntervalAndOnceExpressions) {
    LoopSchedule weekly = daily_at(9, 5);
    weekly.period = PeriodKind::Weekly;
    weekly.weekdays = {1, 5};
    ScheduleCompilation compiled;
    ValidationError error;
    ASSERT_TRUE(compile_schedule(weekly, 0, compiled, &error)) << error.message;
    EXPECT_EQ(compiled.expression, "5 9 * * 1,5");
    // 1970-01-02 was Friday.
    EXPECT_EQ(compiled.next_run_at_ms, kLoopDayMs + 9 * kLoopHourMs + 5 * kLoopMinuteMs);

    LoopSchedule interval;
    interval.kind = ScheduleKind::Interval;
    interval.interval_value = 90;
    interval.interval_unit = IntervalUnit::Minutes;
    interval.anchor_ms = 1'000;
    ASSERT_TRUE(compile_schedule(interval, 500, compiled, &error)) << error.message;
    EXPECT_EQ(compiled.expression, "@every 90m");
    EXPECT_EQ(compiled.next_run_at_ms, 1'000);

    LoopSchedule once;
    once.kind = ScheduleKind::Once;
    once.once_at_ms = 123'456;
    ASSERT_TRUE(compile_schedule(once, 100, compiled, &error)) << error.message;
    EXPECT_EQ(compiled.expression, "@at 123456");
    EXPECT_EQ(compiled.next_run_at_ms, 123'456);
}

TEST(LoopSchedule, PeriodUsesFixedTimezoneOffset) {
    auto schedule = daily_at(9, 0);
    schedule.timezone_offset_minutes = 8 * 60;
    auto next = next_occurrence_ms(schedule, 0);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, kLoopHourMs); // 09:00 UTC+8 = 01:00 UTC.
}

TEST(LoopSchedule, WorkdaysSkipWeekend) {
    auto schedule = daily_at(9, 0);
    schedule.period = PeriodKind::Workdays;
    // 1970-01-02 Fri 10:00 -> next occurrence Mon 1970-01-05 09:00.
    const auto after = kLoopDayMs + 10 * kLoopHourMs;
    auto next = next_occurrence_ms(schedule, after);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next, 4 * kLoopDayMs + 9 * kLoopHourMs);
}

TEST(LoopSchedule, IntervalKeepsAnchorAndValidity) {
    LoopSchedule schedule;
    schedule.kind = ScheduleKind::Interval;
    schedule.interval_value = 90;
    schedule.interval_unit = IntervalUnit::Minutes;
    schedule.anchor_ms = 1'000;
    EXPECT_EQ(next_occurrence_ms(schedule, 1'000), 1'000 + 90 * kLoopMinuteMs);

    schedule.valid_from_ms = 1'000 + 100 * kLoopMinuteMs;
    EXPECT_EQ(next_occurrence_ms(schedule, 0), 1'000 + 180 * kLoopMinuteMs);
    schedule.valid_until_ms = 1'000 + 170 * kLoopMinuteMs;
    EXPECT_FALSE(next_occurrence_ms(schedule, 0).has_value());
}

TEST(LoopSchedule, AdvancesMissedOccurrencesWithoutCatchUp) {
    LoopSchedule schedule;
    schedule.kind = ScheduleKind::Interval;
    schedule.interval_value = 30;
    schedule.interval_unit = IntervalUnit::Minutes;
    schedule.anchor_ms = 1'000;
    auto result = advance_missed_occurrences(
        schedule, 1'000, 1'000 + 95 * kLoopMinuteMs);
    EXPECT_EQ(result.missed_count, 4);
    EXPECT_EQ(result.last_missed_at_ms, 1'000 + 90 * kLoopMinuteMs);
    EXPECT_EQ(result.next_run_at_ms, 1'000 + 120 * kLoopMinuteMs);

    LoopSchedule once;
    once.kind = ScheduleKind::Once;
    once.once_at_ms = 5'000;
    result = advance_missed_occurrences(once, 5'000, 6'000);
    EXPECT_EQ(result.missed_count, 1);
    EXPECT_FALSE(result.next_run_at_ms.has_value());
}

TEST(LoopSchedule, DetectsSameWorkspaceConflictAndExemptsNoWorkspace) {
    const auto schedule = daily_at(9, 0);
    auto existing = loop_for_workspace("existing", "hash-a", schedule);
    auto candidate = loop_for_workspace("candidate", "hash-a", schedule);
    auto conflict = find_schedule_conflict(candidate, {existing}, 0);
    ASSERT_TRUE(conflict.has_value());
    EXPECT_EQ(conflict->loop_id, "existing");
    EXPECT_EQ(conflict->first_conflict_at_ms, 9 * kLoopHourMs);

    candidate.workspace_hash.clear();
    candidate.workspace_cwd.clear();
    existing.workspace_hash.clear();
    existing.workspace_cwd.clear();
    EXPECT_FALSE(find_schedule_conflict(candidate, {existing}, 0).has_value());
}

TEST(LoopSchedule, IgnoresDifferentWorkspaceDisabledAndSelf) {
    auto schedule = daily_at(9, 0);
    auto candidate = loop_for_workspace("same-id", "hash-a", schedule);
    auto self = candidate;
    auto other_workspace = loop_for_workspace("other", "hash-b", schedule);
    auto disabled = loop_for_workspace("disabled", "hash-a", schedule);
    disabled.enabled = false;
    EXPECT_FALSE(find_schedule_conflict(
        candidate, {self, other_workspace, disabled}, 0).has_value());
}

TEST(LoopSchedule, ProducesReadableSummaries) {
    auto schedule = daily_at(9, 5);
    schedule.period = PeriodKind::Weekly;
    schedule.weekdays = {1, 5};
    EXPECT_EQ(schedule_summary(schedule), "周一、周五 09:05");

    schedule.kind = ScheduleKind::Interval;
    schedule.interval_value = 2;
    schedule.interval_unit = IntervalUnit::Hours;
    EXPECT_EQ(schedule_summary(schedule), "每 2 小时");
}
