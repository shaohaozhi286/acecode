#include <gtest/gtest.h>

#include "loop/loop_types.hpp"

using namespace acecode::loop;

namespace {

LoopDefinition valid_loop() {
    LoopDefinition value;
    value.name = " 每日巡检 ";
    value.prompt = " 检查当前代码并报告问题。 ";
    value.model_name = "default-model";
    value.permission_mode = "yolo";
    value.schedule.kind = ScheduleKind::Period;
    value.schedule.period = PeriodKind::Workdays;
    value.schedule.hour = 9;
    value.schedule.minute = 30;
    return value;
}

} // namespace

TEST(LoopTypes, ParsesAndNormalizesDefinition) {
    nlohmann::json body{
        {"name", " 每日代码健康巡检 "},
        {"prompt", " 检查仓库状态。 "},
        {"model_name", "model-a"},
        {"permission_mode", "yolo"},
        {"workspace_hash", ""},
        {"workspace_cwd", ""},
        {"use_worktree", true},
        {"schedule", {
            {"kind", "period"},
            {"period", "weekly"},
            {"weekdays", {5, 1, 1}},
            {"hour", 9},
            {"minute", 0},
            {"timezone_offset_minutes", 480},
        }},
    };
    LoopDefinition parsed;
    ValidationError error;
    ASSERT_TRUE(loop_from_json(body, parsed, &error)) << error.message;
    EXPECT_EQ(parsed.name, "每日代码健康巡检");
    EXPECT_EQ(parsed.prompt, "检查仓库状态。");
    EXPECT_TRUE(parsed.use_worktree);
    EXPECT_EQ(parsed.schedule.weekdays, (std::vector<int>{1, 5}));
    EXPECT_EQ(parsed.schedule.timezone_offset_minutes, 480);

    auto json = loop_to_json(parsed);
    EXPECT_TRUE(json["use_worktree"].get<bool>());
    EXPECT_EQ(json["schedule"]["kind"], "period");
    EXPECT_FALSE(json.contains("schedule_expr"));
}

TEST(LoopTypes, WorktreeDefaultsOffAndRequiresBoolean) {
    auto body = loop_to_json(valid_loop());
    EXPECT_FALSE(body["use_worktree"].get<bool>());

    body["use_worktree"] = "yes";
    LoopDefinition parsed;
    ValidationError error;
    EXPECT_FALSE(loop_from_json(body, parsed, &error));
    EXPECT_EQ(error.code, "INVALID_USE_WORKTREE");
    EXPECT_EQ(error.field, "use_worktree");
}

TEST(LoopTypes, RejectsMissingPromptAndInvalidPermission) {
    auto value = valid_loop();
    value.prompt = "  ";
    normalize_loop_definition(value);
    ValidationError error;
    EXPECT_FALSE(validate_loop_definition(value, &error));
    EXPECT_EQ(error.code, "MISSING_PROMPT");

    value = valid_loop();
    value.permission_mode = "accept-edits";
    EXPECT_FALSE(validate_loop_definition(value, &error));
    EXPECT_EQ(error.code, "INVALID_PERMISSION_MODE");
}

TEST(LoopTypes, RequiresCompleteWorkspacePair) {
    auto value = valid_loop();
    value.workspace_hash = "hash";
    ValidationError error;
    EXPECT_FALSE(validate_loop_definition(value, &error));
    EXPECT_EQ(error.code, "INVALID_WORKSPACE");
}

TEST(LoopTypes, ValidatesScheduleVariants) {
    ValidationError error;
    LoopSchedule weekly;
    weekly.kind = ScheduleKind::Period;
    weekly.period = PeriodKind::Weekly;
    EXPECT_FALSE(validate_loop_schedule(weekly, &error));
    EXPECT_EQ(error.code, "MISSING_WEEKDAY");

    LoopSchedule interval;
    interval.kind = ScheduleKind::Interval;
    interval.interval_value = 90;
    interval.interval_unit = IntervalUnit::Minutes;
    interval.anchor_ms = 1000;
    EXPECT_TRUE(validate_loop_schedule(interval, &error));

    LoopSchedule once;
    once.kind = ScheduleKind::Once;
    once.once_at_ms = 2000;
    once.valid_from_ms = 3000;
    once.valid_until_ms = 1000;
    EXPECT_FALSE(validate_loop_schedule(once, &error));
    EXPECT_EQ(error.code, "INVALID_VALIDITY_WINDOW");
}

TEST(LoopTypes, RunJsonUsesCanonicalStatus) {
    LoopRun run;
    run.id = "run-1";
    run.loop_id = "loop-1";
    run.status = RunStatus::WaitingUser;
    run.scheduled_at_ms = 42;
    auto json = run_to_json(run);
    EXPECT_EQ(json["status"], "waiting_user");
    EXPECT_TRUE(json["started_at_ms"].is_null());
    EXPECT_EQ(parse_run_status("missed"), RunStatus::Missed);
}
