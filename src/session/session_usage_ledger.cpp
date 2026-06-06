#include "session_usage_ledger.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace acecode {
namespace {

constexpr std::int64_t kDayMs = 86'400'000;
constexpr int kDefaultDays = 30;
constexpr int kMaxDays = 366;

std::int64_t timezone_offset_ms(int timezone_offset_minutes) {
    return static_cast<std::int64_t>(timezone_offset_minutes) * 60'000;
}

std::int64_t floor_day(std::int64_t timestamp_ms, int timezone_offset_minutes) {
    if (timestamp_ms < 0) return 0;
    const auto adjusted = timestamp_ms - timezone_offset_ms(timezone_offset_minutes);
    if (adjusted < 0) return 0;
    return adjusted / kDayMs;
}

std::tm utc_tm_from_ms(std::int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &seconds);
#else
    gmtime_r(&seconds, &tm_buf);
#endif
    return tm_buf;
}

void add_totals(UsageTotals& totals, const TokenUsage& usage) {
    totals.prompt_tokens += usage.prompt_tokens;
    totals.completion_tokens += usage.completion_tokens;
    totals.total_tokens += usage.total_tokens;
    totals.cache_read_tokens += usage.cache_read_tokens;
    totals.cache_write_tokens += usage.cache_write_tokens;
    totals.reasoning_tokens += usage.reasoning_tokens;
}

void add_totals(UsageTotals& totals, const UsageTotals& delta) {
    totals.prompt_tokens += delta.prompt_tokens;
    totals.completion_tokens += delta.completion_tokens;
    totals.total_tokens += delta.total_tokens;
    totals.cache_read_tokens += delta.cache_read_tokens;
    totals.cache_write_tokens += delta.cache_write_tokens;
    totals.reasoning_tokens += delta.reasoning_tokens;
}

std::int64_t read_i64(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return 0;
    if (!it->is_number_integer() && !it->is_number_unsigned()) return 0;
    return std::max<std::int64_t>(0, it->get<std::int64_t>());
}

std::string read_string(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string model_label_for(const UsageLedgerRecord& record) {
    if (!record.model_preset.empty()) return record.model_preset;
    if (!record.model.empty()) return record.model;
    if (!record.provider.empty()) return record.provider;
    return "unknown";
}

std::string model_key_for(const UsageLedgerRecord& record) {
    return record.provider + "\n" + record.model + "\n" + record.model_preset;
}

nlohmann::json totals_to_json(const UsageTotals& totals) {
    return nlohmann::json{
        {"prompt_tokens", totals.prompt_tokens},
        {"completion_tokens", totals.completion_tokens},
        {"total_tokens", totals.total_tokens},
        {"cache_read_tokens", totals.cache_read_tokens},
        {"cache_write_tokens", totals.cache_write_tokens},
        {"reasoning_tokens", totals.reasoning_tokens},
    };
}

nlohmann::json session_count_json(const std::set<std::string>& session_ids) {
    return static_cast<std::int64_t>(session_ids.size());
}

} // namespace

std::string usage_ledger_path(const std::string& project_dir) {
    return path_to_utf8(path_from_utf8(project_dir) / "usage.jsonl");
}

std::int64_t usage_now_unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

std::string usage_iso8601_from_unix_ms(std::int64_t timestamp_ms) {
    auto tm_buf = utc_tm_from_ms(timestamp_ms);
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_buf.tm_year + 1900)
        << '-' << std::setw(2) << (tm_buf.tm_mon + 1)
        << '-' << std::setw(2) << tm_buf.tm_mday
        << 'T' << std::setw(2) << tm_buf.tm_hour
        << ':' << std::setw(2) << tm_buf.tm_min
        << ':' << std::setw(2) << tm_buf.tm_sec
        << 'Z';
    return oss.str();
}

std::string usage_date_key_from_unix_ms(std::int64_t timestamp_ms) {
    auto tm_buf = utc_tm_from_ms(timestamp_ms);
    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_buf.tm_year + 1900)
        << '-' << std::setw(2) << (tm_buf.tm_mon + 1)
        << '-' << std::setw(2) << tm_buf.tm_mday;
    return oss.str();
}

std::string usage_date_key_from_day(std::int64_t day) {
    return usage_date_key_from_unix_ms(day * kDayMs);
}

nlohmann::json usage_ledger_record_to_json(const UsageLedgerRecord& record) {
    return nlohmann::json{
        {"version", record.version <= 0 ? 1 : record.version},
        {"timestamp_ms", record.timestamp_ms},
        {"timestamp", record.timestamp.empty()
            ? usage_iso8601_from_unix_ms(record.timestamp_ms)
            : record.timestamp},
        {"session_id", record.session_id},
        {"cwd", record.cwd},
        {"provider", record.provider},
        {"model", record.model},
        {"model_preset", record.model_preset},
        {"surface", record.surface},
        {"prompt_tokens", record.usage.prompt_tokens},
        {"completion_tokens", record.usage.completion_tokens},
        {"total_tokens", record.usage.total_tokens},
        {"cache_read_tokens", record.usage.cache_read_tokens},
        {"cache_write_tokens", record.usage.cache_write_tokens},
        {"reasoning_tokens", record.usage.reasoning_tokens},
        {"has_data", record.usage.has_data},
    };
}

bool usage_ledger_record_from_json(const nlohmann::json& j, UsageLedgerRecord& out) {
    if (!j.is_object()) return false;
    UsageLedgerRecord record;
    record.version = static_cast<int>(read_i64(j, "version"));
    if (record.version <= 0) record.version = 1;
    record.timestamp_ms = read_i64(j, "timestamp_ms");
    record.timestamp = read_string(j, "timestamp");
    record.session_id = read_string(j, "session_id");
    record.cwd = read_string(j, "cwd");
    record.provider = read_string(j, "provider");
    record.model = read_string(j, "model");
    record.model_preset = read_string(j, "model_preset");
    record.surface = read_string(j, "surface");
    record.usage.prompt_tokens = static_cast<int>(read_i64(j, "prompt_tokens"));
    record.usage.completion_tokens = static_cast<int>(read_i64(j, "completion_tokens"));
    record.usage.total_tokens = static_cast<int>(read_i64(j, "total_tokens"));
    record.usage.cache_read_tokens = static_cast<int>(read_i64(j, "cache_read_tokens"));
    record.usage.cache_write_tokens = static_cast<int>(read_i64(j, "cache_write_tokens"));
    record.usage.reasoning_tokens = static_cast<int>(read_i64(j, "reasoning_tokens"));
    record.usage.has_data = j.value("has_data", false);
    if (record.timestamp_ms <= 0 || record.session_id.empty()) return false;
    out = std::move(record);
    return true;
}

void append_usage_ledger_record(const std::string& project_dir,
                                const UsageLedgerRecord& record) {
    static std::mutex append_mu;
    std::lock_guard<std::mutex> lk(append_mu);

    std::error_code ec;
    fs::create_directories(path_from_utf8(project_dir), ec);

    auto json_record = usage_ledger_record_to_json(record).dump();
    json_record.push_back('\n');

    std::ofstream ofs(path_from_utf8(usage_ledger_path(project_dir)),
                      std::ios::binary | std::ios::app);
    if (!ofs.is_open()) return;
    ofs.write(json_record.data(), static_cast<std::streamsize>(json_record.size()));
    ofs.flush();
}

std::vector<UsageLedgerRecord> load_usage_ledger_records(
    const std::string& project_dir) {
    std::vector<UsageLedgerRecord> records;
    std::ifstream ifs(path_from_utf8(usage_ledger_path(project_dir)), std::ios::binary);
    if (!ifs.is_open()) return records;

    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            UsageLedgerRecord record;
            if (usage_ledger_record_from_json(nlohmann::json::parse(line), record)) {
                records.push_back(std::move(record));
            }
        } catch (...) {
            // Keep ledger reads tolerant of interrupted or malformed writes.
        }
    }
    return records;
}

UsageAggregate aggregate_usage_ledgers(
    const std::vector<UsageLedgerScope>& scopes,
    const UsageLedgerQuery& query) {
    UsageAggregate aggregate;
    aggregate.days = std::clamp(query.days <= 0 ? kDefaultDays : query.days, 1, kMaxDays);
    aggregate.timezone_offset_minutes = query.timezone_offset_minutes;
    const auto now_ms = query.now_ms > 0 ? query.now_ms : usage_now_unix_ms();
    const auto offset_ms = timezone_offset_ms(query.timezone_offset_minutes);
    const auto end_day = floor_day(now_ms, query.timezone_offset_minutes);
    const auto start_day = end_day - aggregate.days + 1;
    aggregate.period_start_ms = start_day * kDayMs + offset_ms;
    aggregate.period_end_ms = (end_day + 1) * kDayMs + offset_ms - 1;

    std::unordered_map<std::string, std::size_t> daily_index;
    aggregate.daily.reserve(static_cast<std::size_t>(aggregate.days));
    for (int i = 0; i < aggregate.days; ++i) {
        const auto day = start_day + i;
        UsageDailyBucket bucket;
        bucket.day_start_ms = day * kDayMs + offset_ms;
        bucket.date = usage_date_key_from_day(day);
        daily_index[bucket.date] = aggregate.daily.size();
        aggregate.daily.push_back(std::move(bucket));
    }

    std::map<std::string, UsageModelBucket> model_buckets;
    std::map<std::string, UsageWorkspaceBucket> workspace_buckets;

    for (const auto& scope : scopes) {
        if (!query.workspace_hash.empty() &&
            scope.workspace_hash != query.workspace_hash) {
            continue;
        }

        UsageWorkspaceBucket workspace;
        workspace.workspace_hash = scope.workspace_hash;
        workspace.workspace_name = scope.workspace_name.empty()
            ? (scope.cwd.empty() ? scope.workspace_hash : scope.cwd)
            : scope.workspace_name;
        workspace.cwd = scope.cwd;

        for (const auto& record : load_usage_ledger_records(scope.project_dir)) {
            const auto record_day = floor_day(record.timestamp_ms, query.timezone_offset_minutes);
            if (record_day < start_day || record_day > end_day) continue;

            aggregate.records++;
            if (!record.usage.has_data) aggregate.estimated_records++;
            add_totals(aggregate.totals, record.usage);
            if (!record.session_id.empty()) aggregate.session_ids.insert(record.session_id);

            auto date = usage_date_key_from_day(record_day);
            auto day_it = daily_index.find(date);
            if (day_it != daily_index.end()) {
                auto& day = aggregate.daily[day_it->second];
                day.records++;
                if (!record.usage.has_data) day.estimated_records++;
                add_totals(day.totals, record.usage);
                if (!record.session_id.empty()) day.session_ids.insert(record.session_id);
            }

            auto key = model_key_for(record);
            auto& model = model_buckets[key];
            if (model.label.empty()) {
                model.provider = record.provider;
                model.model = record.model;
                model.model_preset = record.model_preset;
                model.label = model_label_for(record);
            }
            model.records++;
            if (!record.usage.has_data) model.estimated_records++;
            add_totals(model.totals, record.usage);
            if (!record.session_id.empty()) model.session_ids.insert(record.session_id);

            workspace.records++;
            if (!record.usage.has_data) workspace.estimated_records++;
            add_totals(workspace.totals, record.usage);
            if (!record.session_id.empty()) workspace.session_ids.insert(record.session_id);
        }

        if (workspace.records > 0) {
            auto key = scope.workspace_hash.empty() ? scope.project_dir : scope.workspace_hash;
            auto& existing = workspace_buckets[key];
            if (existing.workspace_hash.empty()) {
                existing.workspace_hash = workspace.workspace_hash;
                existing.workspace_name = workspace.workspace_name;
                existing.cwd = workspace.cwd;
            }
            existing.records += workspace.records;
            existing.estimated_records += workspace.estimated_records;
            add_totals(existing.totals, workspace.totals);
            existing.session_ids.insert(workspace.session_ids.begin(), workspace.session_ids.end());
        }
    }

    for (auto& [_, bucket] : model_buckets) {
        aggregate.models.push_back(std::move(bucket));
    }
    for (auto& [_, bucket] : workspace_buckets) {
        aggregate.workspaces.push_back(std::move(bucket));
    }

    auto by_total_desc = [](const auto& a, const auto& b) {
        if (a.totals.total_tokens != b.totals.total_tokens) {
            return a.totals.total_tokens > b.totals.total_tokens;
        }
        return a.records > b.records;
    };
    std::sort(aggregate.models.begin(), aggregate.models.end(), by_total_desc);
    std::sort(aggregate.workspaces.begin(), aggregate.workspaces.end(), by_total_desc);
    return aggregate;
}

nlohmann::json usage_aggregate_to_json(const UsageAggregate& aggregate) {
    nlohmann::json daily = nlohmann::json::array();
    for (const auto& bucket : aggregate.daily) {
        daily.push_back({
            {"date", bucket.date},
            {"day_start_ms", bucket.day_start_ms},
            {"tokens", bucket.totals.total_tokens},
            {"records", bucket.records},
            {"estimated_records", bucket.estimated_records},
            {"session_count", session_count_json(bucket.session_ids)},
            {"totals", totals_to_json(bucket.totals)},
        });
    }

    nlohmann::json models = nlohmann::json::array();
    for (const auto& bucket : aggregate.models) {
        models.push_back({
            {"provider", bucket.provider},
            {"model", bucket.model},
            {"model_preset", bucket.model_preset},
            {"label", bucket.label},
            {"records", bucket.records},
            {"estimated_records", bucket.estimated_records},
            {"session_count", session_count_json(bucket.session_ids)},
            {"totals", totals_to_json(bucket.totals)},
        });
    }

    nlohmann::json workspaces = nlohmann::json::array();
    for (const auto& bucket : aggregate.workspaces) {
        workspaces.push_back({
            {"workspace_hash", bucket.workspace_hash},
            {"workspace_name", bucket.workspace_name},
            {"cwd", bucket.cwd},
            {"records", bucket.records},
            {"estimated_records", bucket.estimated_records},
            {"session_count", session_count_json(bucket.session_ids)},
            {"totals", totals_to_json(bucket.totals)},
        });
    }

    return nlohmann::json{
        {"summary", {
            {"records", aggregate.records},
            {"estimated_records", aggregate.estimated_records},
            {"session_count", session_count_json(aggregate.session_ids)},
            {"totals", totals_to_json(aggregate.totals)},
        }},
        {"daily", daily},
        {"models", models},
        {"workspaces", workspaces},
        {"metadata", {
            {"days", aggregate.days},
            {"period_start_ms", aggregate.period_start_ms},
            {"period_end_ms", aggregate.period_end_ms},
            {"period_start", usage_iso8601_from_unix_ms(aggregate.period_start_ms)},
            {"period_end", usage_iso8601_from_unix_ms(aggregate.period_end_ms)},
            {"timezone_offset_minutes", aggregate.timezone_offset_minutes},
            {"forward_only", true},
        }},
    };
}

} // namespace acecode
