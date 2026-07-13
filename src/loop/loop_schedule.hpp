#pragma once

#include "loop_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::loop {

constexpr std::int64_t kLoopMinuteMs = 60'000;
constexpr std::int64_t kLoopHourMs = 60 * kLoopMinuteMs;
constexpr std::int64_t kLoopDayMs = 24 * kLoopHourMs;

struct ScheduleCompilation {
    std::string expression;
    std::optional<std::int64_t> next_run_at_ms;
};

struct MissedAdvance {
    std::int64_t missed_count = 0;
    std::optional<std::int64_t> last_missed_at_ms;
    std::optional<std::int64_t> next_run_at_ms;
};

struct ScheduleConflict {
    std::string loop_id;
    std::string loop_name;
    std::int64_t first_conflict_at_ms = 0;
};

int current_timezone_offset_minutes(std::int64_t timestamp_ms);
std::int64_t interval_duration_ms(const LoopSchedule& schedule);

bool compile_schedule(const LoopSchedule& schedule,
                      std::int64_t now_ms,
                      ScheduleCompilation& out,
                      ValidationError* error = nullptr);

// Returns the first occurrence strictly after `after_ms`.
std::optional<std::int64_t> next_occurrence_ms(const LoopSchedule& schedule,
                                               std::int64_t after_ms);

MissedAdvance advance_missed_occurrences(const LoopSchedule& schedule,
                                         std::int64_t first_due_ms,
                                         std::int64_t now_ms);

std::string schedule_summary(const LoopSchedule& schedule);

std::optional<ScheduleConflict> find_schedule_conflict(
    const LoopDefinition& candidate,
    const std::vector<LoopDefinition>& existing,
    std::int64_t now_ms,
    std::int64_t horizon_ms = 366 * kLoopDayMs);

} // namespace acecode::loop
