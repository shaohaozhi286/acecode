#pragma once

#include "../provider/llm_provider.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

struct UsageLedgerRecord {
    int version = 1;
    std::int64_t timestamp_ms = 0;
    std::string timestamp;
    std::string session_id;
    std::string cwd;
    std::string provider;
    std::string model;
    std::string model_preset;
    std::string surface;
    TokenUsage usage;
};

struct UsageLedgerScope {
    std::string project_dir;
    std::string workspace_hash;
    std::string workspace_name;
    std::string cwd;
};

struct UsageLedgerQuery {
    int days = 30;
    std::int64_t now_ms = 0;
    int timezone_offset_minutes = 0;
    std::string workspace_hash;
};

struct UsageTotals {
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::int64_t total_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_write_tokens = 0;
    std::int64_t reasoning_tokens = 0;
};

struct UsageDailyBucket {
    std::string date;
    std::int64_t day_start_ms = 0;
    UsageTotals totals;
    int records = 0;
    int estimated_records = 0;
    std::set<std::string> session_ids;
};

struct UsageModelBucket {
    std::string provider;
    std::string model;
    std::string model_preset;
    std::string label;
    UsageTotals totals;
    int records = 0;
    int estimated_records = 0;
    std::set<std::string> session_ids;
};

struct UsageWorkspaceBucket {
    std::string workspace_hash;
    std::string workspace_name;
    std::string cwd;
    UsageTotals totals;
    int records = 0;
    int estimated_records = 0;
    std::set<std::string> session_ids;
};

struct UsageAggregate {
    UsageTotals totals;
    int records = 0;
    int estimated_records = 0;
    std::set<std::string> session_ids;
    int days = 30;
    int timezone_offset_minutes = 0;
    std::int64_t period_start_ms = 0;
    std::int64_t period_end_ms = 0;
    std::vector<UsageDailyBucket> daily;
    std::vector<UsageModelBucket> models;
    std::vector<UsageWorkspaceBucket> workspaces;
};

std::string usage_ledger_path(const std::string& project_dir);
std::int64_t usage_now_unix_ms();
std::string usage_iso8601_from_unix_ms(std::int64_t timestamp_ms);
std::string usage_date_key_from_unix_ms(std::int64_t timestamp_ms);

nlohmann::json usage_ledger_record_to_json(const UsageLedgerRecord& record);
bool usage_ledger_record_from_json(const nlohmann::json& j, UsageLedgerRecord& out);

void append_usage_ledger_record(const std::string& project_dir,
                                const UsageLedgerRecord& record);
std::vector<UsageLedgerRecord> load_usage_ledger_records(
    const std::string& project_dir);

UsageAggregate aggregate_usage_ledgers(
    const std::vector<UsageLedgerScope>& scopes,
    const UsageLedgerQuery& query = {});
nlohmann::json usage_aggregate_to_json(const UsageAggregate& aggregate);

} // namespace acecode
