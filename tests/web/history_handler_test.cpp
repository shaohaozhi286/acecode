// 覆盖 src/web/handlers/history_handler.cpp。daemon 与 TUI 共享同一份
// <project_dir>/input_history.jsonl —— 一旦回归:
//   - load 返回顺序错 → 浏览器 ↑/↓ 翻历史方向反了
//   - max 截断错 → 拿到旧条目而非最新
//   - enabled=false 没静默 → daemon 写盘污染 TUI 不该有的历史

#include <gtest/gtest.h>

#include "web/handlers/history_handler.hpp"

#include "config/config.hpp"
#include "history/input_history_store.hpp"
#include "session/session_storage.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

class HistoryHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 用临时 cwd:每条用例一个独立目录,避免污染。InputHistoryStore::file_path
        // 拼 <project_dir>/input_history.jsonl,project_dir 由 SessionStorage 推导。
        std::random_device rd;
        std::mt19937_64 gen(rd());
        cwd_ = (fs::temp_directory_path() /
                ("acecode_history_test_" + std::to_string(gen()))).string();
        fs::create_directories(cwd_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(cwd_, ec);
        // 也清掉 SessionStorage 在 ~/.acecode/projects 下生成的 project_dir(per-cwd hash)
        auto pd = acecode::SessionStorage::get_project_dir(cwd_);
        fs::remove_all(pd, ec);
    }
    std::string cwd_;
};

acecode::InputHistoryConfig enabled_cfg(int max = 10) {
    acecode::InputHistoryConfig cfg;
    cfg.enabled = true;
    cfg.max_entries = max;
    return cfg;
}

acecode::InputHistoryConfig disabled_cfg() {
    acecode::InputHistoryConfig cfg;
    cfg.enabled = false;
    return cfg;
}

} // namespace

// 场景: 空历史 → 空数组,不抛错。
TEST_F(HistoryHandlerTest, EmptyHistoryReturnsEmptyArray) {
    auto j = acecode::web::load_history(cwd_, 0, enabled_cfg());
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);
}

// 场景: append 后 load 看到对应条目。
TEST_F(HistoryHandlerTest, AppendThenLoad) {
    acecode::web::append_history(cwd_, "hello world", enabled_cfg());
    acecode::web::append_history(cwd_, "second", enabled_cfg());
    auto j = acecode::web::load_history(cwd_, 0, enabled_cfg());
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0], "hello world");
    EXPECT_EQ(j[1], "second");
}

// 场景: max>0 截断 — 保留最新 N 条。
TEST_F(HistoryHandlerTest, MaxTruncatesToLatestN) {
    auto cfg = enabled_cfg(100); // max_entries 大 — 不在 append 时截
    for (int i = 0; i < 5; ++i) {
        acecode::web::append_history(cwd_, "entry-" + std::to_string(i), cfg);
    }
    auto j = acecode::web::load_history(cwd_, 3, cfg);
    ASSERT_EQ(j.size(), 3u);
    EXPECT_EQ(j[0], "entry-2");
    EXPECT_EQ(j[1], "entry-3");
    EXPECT_EQ(j[2], "entry-4");
}

// 场景: max=0 = 不限。返回全部条目。
TEST_F(HistoryHandlerTest, MaxZeroReturnsAll) {
    auto cfg = enabled_cfg(100);
    for (int i = 0; i < 5; ++i) {
        acecode::web::append_history(cwd_, "x" + std::to_string(i), cfg);
    }
    auto j = acecode::web::load_history(cwd_, 0, cfg);
    EXPECT_EQ(j.size(), 5u);
}

// 场景: enabled=false → load 返空数组,append 静默不写盘。
TEST_F(HistoryHandlerTest, DisabledIsSilent) {
    auto cfg = disabled_cfg();
    acecode::web::append_history(cwd_, "should-not-write", cfg);
    auto j = acecode::web::load_history(cwd_, 0, cfg);
    EXPECT_EQ(j.size(), 0u);
    // 即便切换回 enabled,文件应不存在(没写过)
    auto j2 = acecode::web::load_history(cwd_, 0, enabled_cfg());
    EXPECT_EQ(j2.size(), 0u);
}

// 场景: max_entries hard cap — append 时自动截断,即使 caller 不传 max。
TEST_F(HistoryHandlerTest, AppendRespectsMaxEntriesCap) {
    auto cfg = enabled_cfg(3);
    for (int i = 0; i < 6; ++i) {
        acecode::web::append_history(cwd_, "e" + std::to_string(i), cfg);
    }
    auto j = acecode::web::load_history(cwd_, 0, cfg);
    EXPECT_EQ(j.size(), 3u);
    // 最新 3 条
    EXPECT_EQ(j[0], "e3");
    EXPECT_EQ(j[1], "e4");
    EXPECT_EQ(j[2], "e5");
}
