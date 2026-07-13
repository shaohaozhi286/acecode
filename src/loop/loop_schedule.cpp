#include "loop_schedule.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace acecode::loop {

namespace {

std::int64_t floor_div(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    std::int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) --quotient;
    return quotient;
}

int positive_mod(std::int64_t value, int modulus) {
    int result = static_cast<int>(value % modulus);
    return result < 0 ? result + modulus : result;
}

bool day_matches(const LoopSchedule& schedule, std::int64_t epoch_day) {
    const int weekday = positive_mod(epoch_day + 4, 7); // 1970-01-01 was Thursday.
    switch (schedule.period) {
    case PeriodKind::Daily:
        return true;
    case PeriodKind::Workdays:
        return weekday >= 1 && weekday <= 5;
    case PeriodKind::Weekly:
        return std::find(schedule.weekdays.begin(), schedule.weekdays.end(), weekday) !=
               schedule.weekdays.end();
    }
    return false;
}

bool within_validity(const LoopSchedule& schedule, std::int64_t timestamp_ms) {
    if (schedule.valid_from_ms && timestamp_ms < *schedule.valid_from_ms) return false;
    if (schedule.valid_until_ms && timestamp_ms > *schedule.valid_until_ms) return false;
    return true;
}

void set_error(ValidationError* error,
               const std::string& code,
               const std::string& field,
               const std::string& message) {
    if (!error) return;
    error->code = code;
    error->field = field;
    error->message = message;
}

std::string two_digits(int value) {
    std::ostringstream out;
    out << std::setw(2) << std::setfill('0') << value;
    return out.str();
}

std::string weekday_label(int day) {
    static const std::array<const char*, 7> labels{
        "周日", "周一", "周二", "周三", "周四", "周五", "周六"
    };
    return day >= 0 && day < static_cast<int>(labels.size()) ? labels[day] : "?";
}

std::string workspace_key(const LoopDefinition& value) {
    if (!value.workspace_hash.empty()) return "hash:" + value.workspace_hash;
    if (!value.workspace_cwd.empty()) return "cwd:" + value.workspace_cwd;
    return {};
}

std::int64_t minute_bucket(std::int64_t timestamp_ms) {
    return floor_div(timestamp_ms, kLoopMinuteMs);
}

std::optional<std::int64_t> first_occurrence_at_or_after(const LoopSchedule& schedule,
                                                        std::int64_t timestamp_ms) {
    if (timestamp_ms == std::numeric_limits<std::int64_t>::min()) {
        return next_occurrence_ms(schedule, timestamp_ms);
    }
    return next_occurrence_ms(schedule, timestamp_ms - 1);
}

} // namespace

int current_timezone_offset_minutes(std::int64_t timestamp_ms) {
    std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local_tm{};
    std::tm utc_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &seconds);
    gmtime_s(&utc_tm, &seconds);
#else
    localtime_r(&seconds, &local_tm);
    gmtime_r(&seconds, &utc_tm);
#endif
    local_tm.tm_isdst = -1;
    utc_tm.tm_isdst = -1;
    const std::time_t local_as_local = std::mktime(&local_tm);
    const std::time_t utc_as_local = std::mktime(&utc_tm);
    return static_cast<int>(std::difftime(local_as_local, utc_as_local) / 60.0);
}

std::int64_t interval_duration_ms(const LoopSchedule& schedule) {
    if (schedule.kind != ScheduleKind::Interval || schedule.interval_value <= 0) return 0;
    std::int64_t unit = kLoopMinuteMs;
    if (schedule.interval_unit == IntervalUnit::Hours) unit = kLoopHourMs;
    if (schedule.interval_unit == IntervalUnit::Days) unit = kLoopDayMs;
    if (schedule.interval_value > std::numeric_limits<std::int64_t>::max() / unit) return 0;
    return static_cast<std::int64_t>(schedule.interval_value) * unit;
}

std::optional<std::int64_t> next_occurrence_ms(const LoopSchedule& schedule,
                                               std::int64_t after_ms) {
    if (schedule.kind == ScheduleKind::Once) {
        return schedule.once_at_ms > after_ms && within_validity(schedule, schedule.once_at_ms)
            ? std::optional<std::int64_t>(schedule.once_at_ms)
            : std::nullopt;
    }

    if (schedule.kind == ScheduleKind::Interval) {
        const std::int64_t duration = interval_duration_ms(schedule);
        if (duration <= 0) return std::nullopt;
        std::int64_t lower = after_ms;
        if (schedule.valid_from_ms && lower < *schedule.valid_from_ms - 1) {
            lower = *schedule.valid_from_ms - 1;
        }
        std::int64_t candidate = schedule.anchor_ms;
        if (candidate <= lower) {
            const std::int64_t delta = lower - candidate;
            const std::int64_t steps = delta / duration + 1;
            if (steps > (std::numeric_limits<std::int64_t>::max() - candidate) / duration) {
                return std::nullopt;
            }
            candidate += steps * duration;
        }
        if (!within_validity(schedule, candidate)) return std::nullopt;
        return candidate;
    }

    const std::int64_t offset_ms =
        static_cast<std::int64_t>(schedule.timezone_offset_minutes) * kLoopMinuteMs;
    std::int64_t lower = after_ms;
    if (schedule.valid_from_ms && lower < *schedule.valid_from_ms - 1) {
        lower = *schedule.valid_from_ms - 1;
    }
    const std::int64_t local_lower = lower + offset_ms;
    const std::int64_t start_day = floor_div(local_lower, kLoopDayMs);
    const std::int64_t minute_of_day =
        static_cast<std::int64_t>(schedule.hour * 60 + schedule.minute) * kLoopMinuteMs;

    // All supported period schedules produce an occurrence within seven days.
    for (std::int64_t delta = 0; delta <= 14; ++delta) {
        const std::int64_t day = start_day + delta;
        if (!day_matches(schedule, day)) continue;
        const std::int64_t candidate = day * kLoopDayMs + minute_of_day - offset_ms;
        if (candidate <= lower) continue;
        if (schedule.valid_until_ms && candidate > *schedule.valid_until_ms) return std::nullopt;
        if (within_validity(schedule, candidate)) return candidate;
    }
    return std::nullopt;
}

bool compile_schedule(const LoopSchedule& schedule,
                      std::int64_t now_ms,
                      ScheduleCompilation& out,
                      ValidationError* error) {
    if (!validate_loop_schedule(schedule, error)) return false;
    ScheduleCompilation compiled;
    if (schedule.kind == ScheduleKind::Period) {
        std::ostringstream expr;
        expr << schedule.minute << ' ' << schedule.hour << " * * ";
        if (schedule.period == PeriodKind::Daily) {
            expr << '*';
        } else if (schedule.period == PeriodKind::Workdays) {
            expr << "1-5";
        } else {
            for (std::size_t i = 0; i < schedule.weekdays.size(); ++i) {
                if (i) expr << ',';
                expr << schedule.weekdays[i];
            }
        }
        compiled.expression = expr.str();
    } else if (schedule.kind == ScheduleKind::Interval) {
        const char suffix = schedule.interval_unit == IntervalUnit::Minutes ? 'm'
            : schedule.interval_unit == IntervalUnit::Hours ? 'h' : 'd';
        compiled.expression = "@every " + std::to_string(schedule.interval_value) + suffix;
    } else {
        compiled.expression = "@at " + std::to_string(schedule.once_at_ms);
    }
    compiled.next_run_at_ms = next_occurrence_ms(schedule, now_ms);
    if (!compiled.next_run_at_ms) {
        set_error(error, "NO_FUTURE_OCCURRENCE", "schedule",
                  "schedule has no occurrence after the current time");
        return false;
    }
    out = std::move(compiled);
    return true;
}

MissedAdvance advance_missed_occurrences(const LoopSchedule& schedule,
                                         std::int64_t first_due_ms,
                                         std::int64_t now_ms) {
    MissedAdvance out;
    if (first_due_ms > now_ms) {
        out.next_run_at_ms = first_due_ms;
        return out;
    }

    if (schedule.kind == ScheduleKind::Interval) {
        const std::int64_t duration = interval_duration_ms(schedule);
        if (duration <= 0) return out;
        out.missed_count = (now_ms - first_due_ms) / duration + 1;
        out.last_missed_at_ms = first_due_ms + (out.missed_count - 1) * duration;
        const std::int64_t next = *out.last_missed_at_ms + duration;
        if (within_validity(schedule, next)) out.next_run_at_ms = next;
        return out;
    }

    std::optional<std::int64_t> current = first_due_ms;
    while (current && *current <= now_ms) {
        ++out.missed_count;
        out.last_missed_at_ms = current;
        current = next_occurrence_ms(schedule, *current);
        // Defensive ceiling for corrupted schedules; normal period schedules
        // advance at least one day and once schedules stop immediately.
        if (out.missed_count >= 1'000'000) break;
    }
    if (current && *current > now_ms) out.next_run_at_ms = current;
    return out;
}

std::string schedule_summary(const LoopSchedule& schedule) {
    if (schedule.kind == ScheduleKind::Once) return "单次";
    if (schedule.kind == ScheduleKind::Interval) {
        const char* unit = schedule.interval_unit == IntervalUnit::Minutes ? "分钟"
            : schedule.interval_unit == IntervalUnit::Hours ? "小时" : "天";
        return "每 " + std::to_string(schedule.interval_value) + ' ' + unit;
    }
    const std::string time = two_digits(schedule.hour) + ':' + two_digits(schedule.minute);
    if (schedule.period == PeriodKind::Daily) return "每天 " + time;
    if (schedule.period == PeriodKind::Workdays) return "工作日 " + time;
    std::ostringstream out;
    for (std::size_t i = 0; i < schedule.weekdays.size(); ++i) {
        if (i) out << "、";
        out << weekday_label(schedule.weekdays[i]);
    }
    out << ' ' << time;
    return out.str();
}

std::optional<ScheduleConflict> find_schedule_conflict(
    const LoopDefinition& candidate,
    const std::vector<LoopDefinition>& existing,
    std::int64_t now_ms,
    std::int64_t horizon_ms) {
    const std::string candidate_workspace = workspace_key(candidate);
    if (candidate_workspace.empty() || !candidate.enabled || horizon_ms <= 0) return std::nullopt;
    const std::int64_t end_ms = now_ms > std::numeric_limits<std::int64_t>::max() - horizon_ms
        ? std::numeric_limits<std::int64_t>::max()
        : now_ms + horizon_ms;

    for (const auto& other : existing) {
        if (!other.enabled || (!candidate.id.empty() && candidate.id == other.id) ||
            workspace_key(other) != candidate_workspace) continue;

        auto left = next_occurrence_ms(candidate.schedule, now_ms - 1);
        auto right = next_occurrence_ms(other.schedule, now_ms - 1);
        while (left && right && *left <= end_ms && *right <= end_ms) {
            const auto left_minute = minute_bucket(*left);
            const auto right_minute = minute_bucket(*right);
            if (left_minute == right_minute) {
                return ScheduleConflict{other.id, other.name, std::max(*left, *right)};
            }
            if (left_minute < right_minute) {
                left = next_occurrence_ms(candidate.schedule, *left);
            } else {
                right = next_occurrence_ms(other.schedule, *right);
            }
        }
    }
    return std::nullopt;
}

} // namespace acecode::loop
