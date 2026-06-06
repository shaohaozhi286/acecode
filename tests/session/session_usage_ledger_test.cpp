#include "session/session_usage_ledger.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <utility>

namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& prefix) {
    auto base = fs::temp_directory_path();
    std::random_device rd;
    auto path = base / (prefix + "_" + std::to_string(rd()));
    fs::create_directories(path);
    return path;
}

acecode::UsageLedgerRecord record(std::int64_t timestamp_ms,
                                  std::string session_id,
                                  std::string provider,
                                  std::string model,
                                  int prompt,
                                  int completion,
                                  bool has_data = true) {
    acecode::UsageLedgerRecord r;
    r.timestamp_ms = timestamp_ms;
    r.timestamp = acecode::usage_iso8601_from_unix_ms(timestamp_ms);
    r.session_id = std::move(session_id);
    r.cwd = "C:/work";
    r.provider = std::move(provider);
    r.model = std::move(model);
    r.model_preset = r.model;
    r.surface = "web";
    r.usage.prompt_tokens = prompt;
    r.usage.completion_tokens = completion;
    r.usage.total_tokens = prompt + completion;
    r.usage.cache_read_tokens = prompt / 10;
    r.usage.reasoning_tokens = completion / 2;
    r.usage.has_data = has_data;
    return r;
}

} // namespace

TEST(SessionUsageLedger, AppendLoadAndSkipMalformedLines) {
    auto dir = temp_dir("acecode_usage_ledger");
    const auto project_dir = dir.string();

    acecode::append_usage_ledger_record(project_dir, record(1'779'000'000'000, "s1", "openai", "gpt-4o", 100, 20));
    {
        std::ofstream out(acecode::usage_ledger_path(project_dir), std::ios::binary | std::ios::app);
        out << "not json\n";
        out << "{\"timestamp_ms\":1,\"prompt_tokens\":1}\n";
    }
    acecode::append_usage_ledger_record(project_dir, record(1'779'000'060'000, "s2", "openai", "gpt-4o-mini", 200, 30, false));

    auto loaded = acecode::load_usage_ledger_records(project_dir);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].session_id, "s1");
    EXPECT_TRUE(loaded[0].usage.has_data);
    EXPECT_EQ(loaded[1].session_id, "s2");
    EXPECT_FALSE(loaded[1].usage.has_data);

    fs::remove_all(dir);
}

TEST(SessionUsageLedger, AggregateByDayModelWorkspaceAndEstimateCounts) {
    auto a = temp_dir("acecode_usage_a");
    auto b = temp_dir("acecode_usage_b");
    const std::int64_t day1 = 1'779'000'000'000; // 2026-05-14 UTC
    const std::int64_t day2 = day1 + 86'400'000;
    const std::int64_t old = day1 - 86'400'000 * 40;

    acecode::append_usage_ledger_record(a.string(), record(day1, "s1", "openai", "gpt-4o", 100, 20));
    acecode::append_usage_ledger_record(a.string(), record(day2, "s1", "openai", "gpt-4o", 200, 30, false));
    acecode::append_usage_ledger_record(b.string(), record(day2, "s2", "anthropic", "claude", 50, 10));
    acecode::append_usage_ledger_record(b.string(), record(old, "old", "openai", "old", 999, 1));

    acecode::UsageLedgerQuery query;
    query.days = 2;
    query.now_ms = day2 + 1'000;
    auto aggregate = acecode::aggregate_usage_ledgers({
        {a.string(), "wa", "Workspace A", "C:/a"},
        {b.string(), "wb", "Workspace B", "C:/b"},
    }, query);

    EXPECT_EQ(aggregate.records, 3);
    EXPECT_EQ(aggregate.estimated_records, 1);
    EXPECT_EQ(aggregate.session_ids.size(), 2u);
    EXPECT_EQ(aggregate.totals.prompt_tokens, 350);
    EXPECT_EQ(aggregate.totals.completion_tokens, 60);
    EXPECT_EQ(aggregate.totals.total_tokens, 410);

    ASSERT_EQ(aggregate.daily.size(), 2u);
    EXPECT_EQ(aggregate.daily[0].totals.total_tokens, 120);
    EXPECT_EQ(aggregate.daily[1].totals.total_tokens, 290);
    EXPECT_EQ(aggregate.daily[1].estimated_records, 1);

    ASSERT_EQ(aggregate.models.size(), 2u);
    EXPECT_EQ(aggregate.models[0].label, "gpt-4o");
    EXPECT_EQ(aggregate.models[0].totals.total_tokens, 350);
    EXPECT_EQ(aggregate.models[0].estimated_records, 1);

    ASSERT_EQ(aggregate.workspaces.size(), 2u);
    EXPECT_EQ(aggregate.workspaces[0].workspace_hash, "wa");
    EXPECT_EQ(aggregate.workspaces[0].totals.total_tokens, 350);

    auto json = acecode::usage_aggregate_to_json(aggregate);
    EXPECT_EQ(json["summary"]["records"], 3);
    EXPECT_EQ(json["summary"]["totals"]["reasoning_tokens"], 30);
    EXPECT_TRUE(json["metadata"]["forward_only"]);

    fs::remove_all(a);
    fs::remove_all(b);
}

TEST(SessionUsageLedger, WorkspaceFilterLimitsAggregate) {
    auto a = temp_dir("acecode_usage_filter_a");
    auto b = temp_dir("acecode_usage_filter_b");
    const std::int64_t now = 1'779'000'000'000;
    acecode::append_usage_ledger_record(a.string(), record(now, "s1", "openai", "gpt-4o", 100, 20));
    acecode::append_usage_ledger_record(b.string(), record(now, "s2", "openai", "gpt-4o", 300, 40));

    acecode::UsageLedgerQuery query;
    query.days = 1;
    query.now_ms = now;
    query.workspace_hash = "wb";

    auto aggregate = acecode::aggregate_usage_ledgers({
        {a.string(), "wa", "Workspace A", "C:/a"},
        {b.string(), "wb", "Workspace B", "C:/b"},
    }, query);

    EXPECT_EQ(aggregate.records, 1);
    EXPECT_EQ(aggregate.totals.total_tokens, 340);
    ASSERT_EQ(aggregate.workspaces.size(), 1u);
    EXPECT_EQ(aggregate.workspaces[0].workspace_hash, "wb");

    fs::remove_all(a);
    fs::remove_all(b);
}
