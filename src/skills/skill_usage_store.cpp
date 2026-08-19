#include "skills/skill_usage_store.hpp"

#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr int kStateVersion = 1;
constexpr std::size_t kMaxStateFileBytes = 1024 * 1024;  // 1 MB

// Read the state file into a JSON object. Returns an empty object when the
// file is absent, oversized, corrupted, or has an unsupported version so the
// feature degrades gracefully instead of failing the caller.
nlohmann::json load_state_or_empty(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return nlohmann::json::object();
    if (fs::file_size(path, ec) > kMaxStateFileBytes) {
        LOG_WARN("[skill_usage] state file too large, ignoring");
        return nlohmann::json::object();
    }
    std::ifstream ifs(path);
    if (!ifs) return nlohmann::json::object();
    try {
        auto j = nlohmann::json::parse(ifs);
        if (!j.is_object() || j.value("version", 0) != kStateVersion) {
            LOG_WARN("[skill_usage] state file version mismatch, resetting");
            return nlohmann::json::object();
        }
        return j;
    } catch (const nlohmann::json::exception& e) {
        LOG_WARN("[skill_usage] state file parse error: " +
                 std::string(e.what()));
        return nlohmann::json::object();
    }
}

}  // namespace

SkillUsageStore::SkillUsageStore(std::string state_path)
    : state_path_(std::move(state_path)) {}

bool SkillUsageStore::record(const std::string& skill_name,
                             const std::string& now_iso) {
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    state["version"] = kStateVersion;
    auto& skills = state["skills"];
    if (!skills.is_object()) {
        skills = nlohmann::json::object();
    }
    if (!skills.contains(skill_name)) {
        skills[skill_name] = {{"lastUsedAt", now_iso},
                              {"useCount", 1},
                              {"pinned", false}};
    } else {
        auto& entry = skills[skill_name];
        entry["lastUsedAt"] = now_iso;
        entry["useCount"] = entry.value("useCount", 0u) + 1u;
    }
    return atomic_write_file(state_path_, state.dump(2));
}

bool SkillUsageStore::is_dormant(const std::string& skill_name,
                                 std::int64_t now_epoch_ms,
                                 std::int64_t idle_days_ms) const {
    if (idle_days_ms == 0) return false;
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    auto& skills = state["skills"];
    if (!skills.is_object() || !skills.contains(skill_name)) return false;
    auto& entry = skills[skill_name];
    if (!entry.is_object() || entry.value("pinned", false)) return false;
    const std::string last_used = entry.value("lastUsedAt", "");
    if (last_used.empty()) return false;
    const std::int64_t last_ms = parse_iso8601_to_epoch_ms(last_used);
    if (last_ms == 0) return false;
    return (now_epoch_ms - last_ms) > idle_days_ms;
}

bool SkillUsageStore::set_pinned(const std::string& skill_name, bool pinned) {
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    state["version"] = kStateVersion;
    auto& skills = state["skills"];
    if (!skills.is_object()) {
        skills = nlohmann::json::object();
    }
    if (!skills.contains(skill_name)) {
        skills[skill_name] = {{"lastUsedAt", ""},
                              {"useCount", 0},
                              {"pinned", pinned}};
    } else {
        skills[skill_name]["pinned"] = pinned;
    }
    return atomic_write_file(state_path_, state.dump(2));
}

std::vector<SkillUsageSummary> SkillUsageStore::get_summary(
    std::int64_t now_epoch_ms, std::int64_t idle_days_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto state = load_state_or_empty(state_path_);
    std::vector<SkillUsageSummary> out;
    auto& skills = state["skills"];
    if (!skills.is_object()) return out;
    for (auto& [name, entry] : skills.items()) {
        if (!entry.is_object()) continue;
        SkillUsageSummary s;
        s.name = name;
        s.use_count = entry.value("useCount", 0u);
        s.last_used_at = entry.value("lastUsedAt", "");
        s.pinned = entry.value("pinned", false);
        if (idle_days_ms > 0 && !s.pinned && !s.last_used_at.empty()) {
            const std::int64_t last_ms =
                parse_iso8601_to_epoch_ms(s.last_used_at);
            s.dormant = (last_ms > 0) &&
                        (now_epoch_ms - last_ms) > idle_days_ms;
        }
        out.push_back(std::move(s));
    }
    return out;
}

void SkillUsageStore::reload() {
    // The next access re-reads the file via load_state_or_empty; nothing
    // to invalidate here.
}

std::int64_t parse_iso8601_to_epoch_ms(const std::string& iso) {
    if (iso.empty()) return 0;
    std::tm tm = {};
    std::istringstream ss(iso);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;
    int ms = 0;
    if (ss.peek() == '.') {
        ss.ignore();
        ss >> ms;
    }
    // Treat the parsed time as UTC (the 'Z' suffix / gmtime convention).
    auto tp = std::chrono::system_clock::from_time_t(
                  std::mktime(&tm)) +
              std::chrono::milliseconds(ms);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch())
        .count();
}

}  // namespace acecode
