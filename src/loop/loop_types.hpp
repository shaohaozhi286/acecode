#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::loop {

enum class ScheduleKind { Period, Interval, Once };
enum class PeriodKind { Daily, Workdays, Weekly };
enum class IntervalUnit { Minutes, Hours, Days };
enum class RunStatus { Scheduled, Running, WaitingUser, Completed, Failed, Missed };

struct LoopSchedule {
    ScheduleKind kind = ScheduleKind::Period;

    // Period schedule. weekdays use 0=Sunday ... 6=Saturday.
    PeriodKind period = PeriodKind::Daily;
    std::vector<int> weekdays;
    int hour = 9;
    int minute = 0;

    // Interval schedule. anchor_ms is the first candidate occurrence.
    int interval_value = 1;
    IntervalUnit interval_unit = IntervalUnit::Hours;
    std::int64_t anchor_ms = 0;

    // Once schedule.
    std::int64_t once_at_ms = 0;

    // Wall-clock period schedules use this fixed local offset. The API fills
    // the daemon's current local offset when the client omits it.
    int timezone_offset_minutes = 0;

    std::optional<std::int64_t> valid_from_ms;
    std::optional<std::int64_t> valid_until_ms;
};

struct LoopDefinition {
    std::string id;
    std::string name;
    std::string prompt;
    std::string workspace_hash;
    std::string workspace_cwd;
    std::string model_name;
    std::string permission_mode = "yolo"; // default | yolo
    bool use_worktree = false;
    LoopSchedule schedule;
    std::string schedule_expr;
    std::optional<std::int64_t> next_run_at_ms;
    bool enabled = true;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
};

struct LoopRun {
    std::string id;
    std::string loop_id;
    std::int64_t scheduled_at_ms = 0;
    std::optional<std::int64_t> started_at_ms;
    std::optional<std::int64_t> finished_at_ms;
    RunStatus status = RunStatus::Scheduled;
    std::string reason;
    std::int64_t missed_count = 1;
    std::string session_id;
    std::string worktree_path;
    std::string worktree_branch;
    std::string owner_id;
};

struct ValidationError {
    std::string code;
    std::string field;
    std::string message;
};

const char* to_string(ScheduleKind value);
const char* to_string(PeriodKind value);
const char* to_string(IntervalUnit value);
const char* to_string(RunStatus value);

std::optional<ScheduleKind> parse_schedule_kind(const std::string& value);
std::optional<PeriodKind> parse_period_kind(const std::string& value);
std::optional<IntervalUnit> parse_interval_unit(const std::string& value);
std::optional<RunStatus> parse_run_status(const std::string& value);

std::string trim_loop_text(const std::string& value);
void normalize_loop_definition(LoopDefinition& value);
bool validate_loop_schedule(const LoopSchedule& value, ValidationError* error = nullptr);
bool validate_loop_definition(const LoopDefinition& value, ValidationError* error = nullptr);

nlohmann::json schedule_to_json(const LoopSchedule& value);
bool schedule_from_json(const nlohmann::json& json,
                        LoopSchedule& out,
                        ValidationError* error = nullptr);
nlohmann::json loop_to_json(const LoopDefinition& value, bool include_internal = false);
bool loop_from_json(const nlohmann::json& json,
                    LoopDefinition& out,
                    ValidationError* error = nullptr);
nlohmann::json run_to_json(const LoopRun& value);

} // namespace acecode::loop
