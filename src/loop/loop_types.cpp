#include "loop_types.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace acecode::loop {

namespace {

constexpr std::size_t kMaxNameBytes = 160;
constexpr std::size_t kMaxPromptBytes = 256 * 1024;

void set_error(ValidationError* error,
               const std::string& code,
               const std::string& field,
               const std::string& message) {
    if (!error) return;
    error->code = code;
    error->field = field;
    error->message = message;
}

template <typename T>
bool read_integer(const nlohmann::json& value, const char* key, T& out) {
    if (!value.contains(key) || !value[key].is_number_integer()) return false;
    const auto raw = value[key].get<std::int64_t>();
    if (raw < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
        raw > static_cast<std::int64_t>(std::numeric_limits<T>::max())) return false;
    out = static_cast<T>(raw);
    return true;
}

std::optional<std::int64_t> optional_i64(const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || value[key].is_null()) return std::nullopt;
    if (!value[key].is_number_integer()) return std::nullopt;
    return value[key].get<std::int64_t>();
}

bool valid_optional_i64(const nlohmann::json& value, const char* key) {
    return !value.contains(key) || value[key].is_null() || value[key].is_number_integer();
}

} // namespace

const char* to_string(ScheduleKind value) {
    switch (value) {
    case ScheduleKind::Period: return "period";
    case ScheduleKind::Interval: return "interval";
    case ScheduleKind::Once: return "once";
    }
    return "period";
}

const char* to_string(PeriodKind value) {
    switch (value) {
    case PeriodKind::Daily: return "daily";
    case PeriodKind::Workdays: return "workdays";
    case PeriodKind::Weekly: return "weekly";
    }
    return "daily";
}

const char* to_string(IntervalUnit value) {
    switch (value) {
    case IntervalUnit::Minutes: return "minutes";
    case IntervalUnit::Hours: return "hours";
    case IntervalUnit::Days: return "days";
    }
    return "hours";
}

const char* to_string(RunStatus value) {
    switch (value) {
    case RunStatus::Scheduled: return "scheduled";
    case RunStatus::Running: return "running";
    case RunStatus::WaitingUser: return "waiting_user";
    case RunStatus::Completed: return "completed";
    case RunStatus::Failed: return "failed";
    case RunStatus::Missed: return "missed";
    }
    return "failed";
}

std::optional<ScheduleKind> parse_schedule_kind(const std::string& value) {
    if (value == "period") return ScheduleKind::Period;
    if (value == "interval") return ScheduleKind::Interval;
    if (value == "once") return ScheduleKind::Once;
    return std::nullopt;
}

std::optional<PeriodKind> parse_period_kind(const std::string& value) {
    if (value == "daily") return PeriodKind::Daily;
    if (value == "workdays") return PeriodKind::Workdays;
    if (value == "weekly") return PeriodKind::Weekly;
    return std::nullopt;
}

std::optional<IntervalUnit> parse_interval_unit(const std::string& value) {
    if (value == "minutes") return IntervalUnit::Minutes;
    if (value == "hours") return IntervalUnit::Hours;
    if (value == "days") return IntervalUnit::Days;
    return std::nullopt;
}

std::optional<RunStatus> parse_run_status(const std::string& value) {
    if (value == "scheduled") return RunStatus::Scheduled;
    if (value == "running") return RunStatus::Running;
    if (value == "waiting_user") return RunStatus::WaitingUser;
    if (value == "completed") return RunStatus::Completed;
    if (value == "failed") return RunStatus::Failed;
    if (value == "missed") return RunStatus::Missed;
    return std::nullopt;
}

std::string trim_loop_text(const std::string& value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

void normalize_loop_definition(LoopDefinition& value) {
    value.name = trim_loop_text(value.name);
    value.prompt = trim_loop_text(value.prompt);
    value.workspace_hash = trim_loop_text(value.workspace_hash);
    value.workspace_cwd = trim_loop_text(value.workspace_cwd);
    value.model_name = trim_loop_text(value.model_name);
    value.permission_mode = trim_loop_text(value.permission_mode);
    std::sort(value.schedule.weekdays.begin(), value.schedule.weekdays.end());
    value.schedule.weekdays.erase(
        std::unique(value.schedule.weekdays.begin(), value.schedule.weekdays.end()),
        value.schedule.weekdays.end());
}

bool validate_loop_schedule(const LoopSchedule& value, ValidationError* error) {
    if (value.timezone_offset_minutes < -14 * 60 || value.timezone_offset_minutes > 14 * 60) {
        set_error(error, "INVALID_TIMEZONE", "schedule.timezone_offset_minutes",
                  "timezone offset must be between -840 and 840 minutes");
        return false;
    }
    if (value.valid_from_ms && value.valid_until_ms &&
        *value.valid_until_ms < *value.valid_from_ms) {
        set_error(error, "INVALID_VALIDITY_WINDOW", "schedule.valid_until_ms",
                  "valid_until_ms must not be earlier than valid_from_ms");
        return false;
    }
    switch (value.kind) {
    case ScheduleKind::Period: {
        if (value.hour < 0 || value.hour > 23 || value.minute < 0 || value.minute > 59) {
            set_error(error, "INVALID_TIME", "schedule.time",
                      "period hour/minute is out of range");
            return false;
        }
        if (value.period == PeriodKind::Weekly && value.weekdays.empty()) {
            set_error(error, "MISSING_WEEKDAY", "schedule.weekdays",
                      "weekly schedule requires at least one weekday");
            return false;
        }
        std::set<int> seen;
        for (int day : value.weekdays) {
            if (day < 0 || day > 6 || !seen.insert(day).second) {
                set_error(error, "INVALID_WEEKDAY", "schedule.weekdays",
                          "weekdays must be unique values from 0 through 6");
                return false;
            }
        }
        return true;
    }
    case ScheduleKind::Interval:
        if (value.interval_value <= 0 || value.interval_value > 100000) {
            set_error(error, "INVALID_INTERVAL", "schedule.interval_value",
                      "interval_value must be between 1 and 100000");
            return false;
        }
        if (value.anchor_ms <= 0) {
            set_error(error, "INVALID_ANCHOR", "schedule.anchor_ms",
                      "interval schedule requires a positive anchor_ms");
            return false;
        }
        return true;
    case ScheduleKind::Once:
        if (value.once_at_ms <= 0) {
            set_error(error, "INVALID_ONCE_TIME", "schedule.once_at_ms",
                      "once schedule requires a positive once_at_ms");
            return false;
        }
        return true;
    }
    set_error(error, "INVALID_SCHEDULE", "schedule", "unsupported schedule kind");
    return false;
}

bool validate_loop_definition(const LoopDefinition& value, ValidationError* error) {
    if (trim_loop_text(value.name).empty()) {
        set_error(error, "MISSING_NAME", "name", "LOOP name is required");
        return false;
    }
    if (value.name.size() > kMaxNameBytes) {
        set_error(error, "NAME_TOO_LONG", "name", "LOOP name is too long");
        return false;
    }
    if (trim_loop_text(value.prompt).empty()) {
        set_error(error, "MISSING_PROMPT", "prompt", "LOOP prompt is required");
        return false;
    }
    if (value.prompt.size() > kMaxPromptBytes) {
        set_error(error, "PROMPT_TOO_LONG", "prompt", "LOOP prompt is too long");
        return false;
    }
    if (trim_loop_text(value.model_name).empty()) {
        set_error(error, "MISSING_MODEL", "model_name", "LOOP model is required");
        return false;
    }
    if (value.permission_mode != "default" && value.permission_mode != "yolo") {
        set_error(error, "INVALID_PERMISSION_MODE", "permission_mode",
                  "LOOP permission_mode must be default or yolo");
        return false;
    }
    const bool has_hash = !trim_loop_text(value.workspace_hash).empty();
    const bool has_cwd = !trim_loop_text(value.workspace_cwd).empty();
    if (has_hash != has_cwd) {
        set_error(error, "INVALID_WORKSPACE", "workspace",
                  "workspace_hash and workspace_cwd must both be set or both be empty");
        return false;
    }
    return validate_loop_schedule(value.schedule, error);
}

nlohmann::json schedule_to_json(const LoopSchedule& value) {
    nlohmann::json out{
        {"kind", to_string(value.kind)},
        {"timezone_offset_minutes", value.timezone_offset_minutes},
        {"valid_from_ms", value.valid_from_ms ? nlohmann::json(*value.valid_from_ms) : nlohmann::json(nullptr)},
        {"valid_until_ms", value.valid_until_ms ? nlohmann::json(*value.valid_until_ms) : nlohmann::json(nullptr)},
    };
    if (value.kind == ScheduleKind::Period) {
        out["period"] = to_string(value.period);
        out["weekdays"] = value.weekdays;
        out["hour"] = value.hour;
        out["minute"] = value.minute;
    } else if (value.kind == ScheduleKind::Interval) {
        out["interval_value"] = value.interval_value;
        out["interval_unit"] = to_string(value.interval_unit);
        out["anchor_ms"] = value.anchor_ms;
    } else {
        out["once_at_ms"] = value.once_at_ms;
    }
    return out;
}

bool schedule_from_json(const nlohmann::json& json,
                        LoopSchedule& out,
                        ValidationError* error) {
    if (!json.is_object()) {
        set_error(error, "INVALID_SCHEDULE", "schedule", "schedule must be an object");
        return false;
    }
    if (!json.contains("kind") || !json["kind"].is_string()) {
        set_error(error, "MISSING_SCHEDULE_KIND", "schedule.kind", "schedule kind is required");
        return false;
    }
    auto kind = parse_schedule_kind(json["kind"].get<std::string>());
    if (!kind) {
        set_error(error, "INVALID_SCHEDULE_KIND", "schedule.kind", "unsupported schedule kind");
        return false;
    }
    LoopSchedule parsed;
    parsed.kind = *kind;
    if (json.contains("timezone_offset_minutes")) {
        if (!read_integer(json, "timezone_offset_minutes", parsed.timezone_offset_minutes)) {
            set_error(error, "INVALID_TIMEZONE", "schedule.timezone_offset_minutes",
                      "timezone_offset_minutes must be an integer");
            return false;
        }
    }
    if (!valid_optional_i64(json, "valid_from_ms") ||
        !valid_optional_i64(json, "valid_until_ms")) {
        set_error(error, "INVALID_VALIDITY_WINDOW", "schedule",
                  "validity timestamps must be integers or null");
        return false;
    }
    parsed.valid_from_ms = optional_i64(json, "valid_from_ms");
    parsed.valid_until_ms = optional_i64(json, "valid_until_ms");

    if (parsed.kind == ScheduleKind::Period) {
        if (!json.contains("period") || !json["period"].is_string()) {
            set_error(error, "MISSING_PERIOD", "schedule.period", "period is required");
            return false;
        }
        auto period = parse_period_kind(json["period"].get<std::string>());
        if (!period) {
            set_error(error, "INVALID_PERIOD", "schedule.period", "unsupported period");
            return false;
        }
        parsed.period = *period;
        if (!read_integer(json, "hour", parsed.hour) ||
            !read_integer(json, "minute", parsed.minute)) {
            set_error(error, "INVALID_TIME", "schedule.time", "hour and minute are required integers");
            return false;
        }
        if (json.contains("weekdays")) {
            if (!json["weekdays"].is_array()) {
                set_error(error, "INVALID_WEEKDAY", "schedule.weekdays", "weekdays must be an array");
                return false;
            }
            for (const auto& item : json["weekdays"]) {
                if (!item.is_number_integer()) {
                    set_error(error, "INVALID_WEEKDAY", "schedule.weekdays", "weekday must be an integer");
                    return false;
                }
                parsed.weekdays.push_back(item.get<int>());
            }
        }
        std::sort(parsed.weekdays.begin(), parsed.weekdays.end());
        parsed.weekdays.erase(std::unique(parsed.weekdays.begin(), parsed.weekdays.end()),
                              parsed.weekdays.end());
    } else if (parsed.kind == ScheduleKind::Interval) {
        if (!read_integer(json, "interval_value", parsed.interval_value) ||
            !read_integer(json, "anchor_ms", parsed.anchor_ms) ||
            !json.contains("interval_unit") || !json["interval_unit"].is_string()) {
            set_error(error, "INVALID_INTERVAL", "schedule", "interval fields are incomplete");
            return false;
        }
        auto unit = parse_interval_unit(json["interval_unit"].get<std::string>());
        if (!unit) {
            set_error(error, "INVALID_INTERVAL_UNIT", "schedule.interval_unit",
                      "unsupported interval unit");
            return false;
        }
        parsed.interval_unit = *unit;
    } else if (!read_integer(json, "once_at_ms", parsed.once_at_ms)) {
        set_error(error, "INVALID_ONCE_TIME", "schedule.once_at_ms",
                  "once_at_ms is required");
        return false;
    }
    if (!validate_loop_schedule(parsed, error)) return false;
    out = std::move(parsed);
    return true;
}

nlohmann::json loop_to_json(const LoopDefinition& value, bool include_internal) {
    nlohmann::json out{
        {"id", value.id},
        {"name", value.name},
        {"prompt", value.prompt},
        {"workspace_hash", value.workspace_hash},
        {"workspace_cwd", value.workspace_cwd},
        {"model_name", value.model_name},
        {"permission_mode", value.permission_mode},
        {"use_worktree", value.use_worktree},
        {"schedule", schedule_to_json(value.schedule)},
        {"next_run_at_ms", value.next_run_at_ms ? nlohmann::json(*value.next_run_at_ms) : nlohmann::json(nullptr)},
        {"enabled", value.enabled},
        {"created_at_ms", value.created_at_ms},
        {"updated_at_ms", value.updated_at_ms},
    };
    if (include_internal) out["schedule_expr"] = value.schedule_expr;
    return out;
}

bool loop_from_json(const nlohmann::json& json,
                    LoopDefinition& out,
                    ValidationError* error) {
    if (!json.is_object()) {
        set_error(error, "INVALID_LOOP", "", "LOOP body must be an object");
        return false;
    }
    LoopDefinition parsed;
    auto read_string = [&](const char* key, std::string& target, bool required) {
        if (!json.contains(key)) return !required;
        if (!json[key].is_string()) return false;
        target = json[key].get<std::string>();
        return true;
    };
    if (!read_string("name", parsed.name, true) ||
        !read_string("prompt", parsed.prompt, true) ||
        !read_string("workspace_hash", parsed.workspace_hash, false) ||
        !read_string("workspace_cwd", parsed.workspace_cwd, false) ||
        !read_string("model_name", parsed.model_name, true) ||
        !read_string("permission_mode", parsed.permission_mode, false)) {
        set_error(error, "INVALID_FIELD_TYPE", "", "LOOP string field has an invalid type");
        return false;
    }
    if (json.contains("id") && json["id"].is_string()) parsed.id = json["id"].get<std::string>();
    if (json.contains("enabled")) {
        if (!json["enabled"].is_boolean()) {
            set_error(error, "INVALID_ENABLED", "enabled", "enabled must be boolean");
            return false;
        }
        parsed.enabled = json["enabled"].get<bool>();
    }
    if (json.contains("use_worktree")) {
        if (!json["use_worktree"].is_boolean()) {
            set_error(error, "INVALID_USE_WORKTREE", "use_worktree",
                      "use_worktree must be boolean");
            return false;
        }
        parsed.use_worktree = json["use_worktree"].get<bool>();
    }
    if (!json.contains("schedule") || !schedule_from_json(json["schedule"], parsed.schedule, error)) {
        if (error && error->code.empty()) {
            set_error(error, "MISSING_SCHEDULE", "schedule", "schedule is required");
        }
        return false;
    }
    normalize_loop_definition(parsed);
    if (!validate_loop_definition(parsed, error)) return false;
    out = std::move(parsed);
    return true;
}

nlohmann::json run_to_json(const LoopRun& value) {
    return nlohmann::json{
        {"id", value.id},
        {"loop_id", value.loop_id},
        {"scheduled_at_ms", value.scheduled_at_ms},
        {"started_at_ms", value.started_at_ms ? nlohmann::json(*value.started_at_ms) : nlohmann::json(nullptr)},
        {"finished_at_ms", value.finished_at_ms ? nlohmann::json(*value.finished_at_ms) : nlohmann::json(nullptr)},
        {"status", to_string(value.status)},
        {"reason", value.reason},
        {"missed_count", value.missed_count},
        {"session_id", value.session_id},
        {"worktree_path", value.worktree_path},
        {"worktree_branch", value.worktree_branch},
    };
}

} // namespace acecode::loop
