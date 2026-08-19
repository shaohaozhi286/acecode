#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace acecode {

struct SkillUsageRecord {
    std::string last_used_at;   // ISO8601, e.g. "2026-08-01T10:00:00Z"
    std::uint64_t use_count = 0;
    bool pinned = false;
};

struct SkillUsageSummary {
    std::string name;
    std::uint64_t use_count = 0;
    std::string last_used_at;
    bool pinned = false;
    bool dormant = false;   // 实时判定结果(仅展示用,不落盘)
};

// Manages the skill-usage state file (~/.acecode/.skill_usage_state.json).
// Thread-safe: every public method serializes on an internal mutex.
class SkillUsageStore {
public:
    // state_path: absolute path to the JSON state file.
    // Does not touch disk until the first record()/is_dormant() call.
    explicit SkillUsageStore(std::string state_path);

    // Record one successful use of skill_name at now (ISO8601).
    // Creates the record if absent (use_count = 1); otherwise increments
    // use_count and refreshes last_used_at. Best-effort: returns false on
    // persistence failure, never throws.
    bool record(const std::string& skill_name, const std::string& now_iso);

    // Dormancy predicate:
    //   !pinned && (now_epoch_ms - last_used_epoch_ms) > idle_days_ms
    // Returns false when idle_days_ms == 0 (feature disabled), when the
    // record is absent, or when the timestamp cannot be parsed.
    bool is_dormant(const std::string& skill_name,
                    std::int64_t now_epoch_ms,
                    std::int64_t idle_days_ms) const;

    // Set or clear the pinned flag. Creates a record if absent.
    // Best-effort: returns false on persistence failure, never throws.
    bool set_pinned(const std::string& skill_name, bool pinned);

    // Snapshot of all records for display (TUI/Web). The `dormant` field is
    // computed live from now_epoch_ms / idle_days_ms and is not persisted.
    std::vector<SkillUsageSummary> get_summary(
        std::int64_t now_epoch_ms, std::int64_t idle_days_ms) const;

    // Re-read the state file on the next access (call after config changes).
    void reload();

private:
    std::string state_path_;
    mutable std::mutex mu_;
};

// Parse an ISO8601 timestamp ("2026-08-01T10:00:00Z", optional .ms fraction)
// to epoch milliseconds. Returns 0 on parse failure or empty input.
std::int64_t parse_iso8601_to_epoch_ms(const std::string& iso);

}  // namespace acecode
