#include "skills/skill_usage_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;

fs::path temp_state_file(const std::string& name) {
    fs::path tmp = fs::temp_directory_path() /
        ("acecode_skill_usage_" + name + ".json");
    std::error_code ec;
    fs::remove(tmp, ec);
    return tmp;
}

}  // namespace

TEST(SkillUsageStoreTest, ParseIso8601) {
    const auto ms = acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00Z");
    EXPECT_GT(ms, 0);
    EXPECT_EQ(ms,
              acecode::parse_iso8601_to_epoch_ms("2026-08-01T10:00:00Z"));
    // 无效输入返回 0
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms("not-a-date"));
    EXPECT_EQ(0, acecode::parse_iso8601_to_epoch_ms(""));
}

TEST(SkillUsageStoreTest, RecordCreatesEntry) {
    const fs::path tmp = temp_state_file("record");
    acecode::SkillUsageStore store(tmp.string());

    EXPECT_TRUE(store.record("pdf", "2026-08-01T10:00:00Z"));
    // 新记录应 active
    EXPECT_FALSE(store.is_dormant("pdf", 1785578400000LL, 30 * kDayMs));
    auto s = store.get_summary(1785578400000LL, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].name, "pdf");
    EXPECT_EQ(s[0].use_count, 1u);
    EXPECT_FALSE(s[0].dormant);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, DormantAfterThreshold) {
    const fs::path tmp = temp_state_file("dormant");
    acecode::SkillUsageStore store(tmp.string());

    // 在 t=used_ms 使用,距今 40 天 > 30 天阈值
    const std::int64_t used_ms = 1785578400000LL;  // 2026-08-01T10:00:00Z
    const std::int64_t now_ms = used_ms + 40 * kDayMs;
    EXPECT_TRUE(store.record("xlsx", "2026-08-01T10:00:00Z"));
    EXPECT_TRUE(store.is_dormant("xlsx", now_ms, 30 * kDayMs));
    auto s = store.get_summary(now_ms, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_TRUE(s[0].dormant);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, PinnedSkillNeverDormant) {
    const fs::path tmp = temp_state_file("pinned");
    acecode::SkillUsageStore store(tmp.string());

    EXPECT_TRUE(store.record("pinned_skill", "2026-06-01T10:00:00Z"));
    EXPECT_TRUE(store.set_pinned("pinned_skill", true));
    const std::int64_t now_ms = 1780308000000LL + 60 * kDayMs;
    EXPECT_FALSE(store.is_dormant("pinned_skill", now_ms, 30 * kDayMs));
    auto s = store.get_summary(now_ms, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_FALSE(s[0].dormant);
    EXPECT_TRUE(s[0].pinned);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, IdleDaysZeroDisablesFeature) {
    const fs::path tmp = temp_state_file("zero");
    acecode::SkillUsageStore store(tmp.string());

    EXPECT_TRUE(store.record("skill", "2020-01-01T00:00:00Z"));
    EXPECT_FALSE(store.is_dormant("skill", 1785578400000LL, 0));
    auto s = store.get_summary(1785578400000LL, 0);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_FALSE(s[0].dormant);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, IncrementUseCount) {
    const fs::path tmp = temp_state_file("incr");
    acecode::SkillUsageStore store(tmp.string());

    store.record("pdf", "2026-08-01T10:00:00Z");
    store.record("pdf", "2026-08-02T10:00:00Z");
    store.record("pdf", "2026-08-03T10:00:00Z");
    auto s = store.get_summary(1785578400000LL, 30 * kDayMs);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].use_count, 3u);
    fs::remove(tmp);
}

TEST(SkillUsageStoreTest, CorruptStateDegradesGracefully) {
    const fs::path tmp = temp_state_file("corrupt");
    {
        std::ofstream ofs(tmp);
        ofs << "{ this is not valid json";
    }
    acecode::SkillUsageStore store(tmp.string());

    // 损坏文件:record 仍成功(内部重置),不抛异常
    EXPECT_TRUE(store.record("pdf", "2026-08-01T10:00:00Z"));
    EXPECT_FALSE(store.is_dormant("pdf", 1785578400000LL, 30 * kDayMs));
    fs::remove(tmp);
}
