// 覆盖 src/utils/state_file 中新增的 web_search region 缓存读写。
// 与 state_file_test 同模式,经 set_state_file_path_for_test 把读写路径切到
// 测试临时目录,避免污染真实 ~/.acecode/state.json。
//
// 覆盖项:
//   - 缺失字段 → nullopt
//   - 写后读回(global / cn 两个合法 region)
//   - 写入非法 region 静默拒绝(不破坏文件)
//   - 写入保留其它 unrelated 顶层 key(如 legacy_terminal_hint_shown)
//   - clear 后再读 → nullopt
//   - 损坏 JSON 文件 → 读返回 nullopt(不抛)

#include <gtest/gtest.h>

#include "utils/state_file.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class StateJsonWebSearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto tmp = fs::temp_directory_path() /
                   ("acecode_websearch_state_" + std::to_string(std::rand()));
        fs::create_directories(tmp);
        path_ = (tmp / "state.json").string();
        acecode::set_state_file_path_for_test(path_);
    }
    void TearDown() override {
        acecode::set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(fs::path(path_).parent_path(), ec);
    }

    std::string read_raw() const {
        std::ifstream ifs(path_);
        std::stringstream buf;
        buf << ifs.rdbuf();
        return buf.str();
    }

    std::string path_;
};

} // namespace

// 场景:文件不存在 → 读 nullopt
TEST_F(StateJsonWebSearchTest, MissingFileReturnsNullopt) {
    EXPECT_FALSE(acecode::read_web_search_region_cache().has_value());
}

// 场景:写 global → 立即读回
TEST_F(StateJsonWebSearchTest, WriteThenReadGlobal) {
    acecode::WebSearchRegionCache c;
    c.region = "global";
    c.detected_at_ms = 1714521600000LL;
    acecode::write_web_search_region_cache(c);

    auto got = acecode::read_web_search_region_cache();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->region, "global");
    EXPECT_EQ(got->detected_at_ms, 1714521600000LL);
}

// 场景:写 cn 同样 round-trip
TEST_F(StateJsonWebSearchTest, WriteThenReadCn) {
    acecode::WebSearchRegionCache c;
    c.region = "cn";
    c.detected_at_ms = 0; // 0 也接受
    acecode::write_web_search_region_cache(c);

    auto got = acecode::read_web_search_region_cache();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->region, "cn");
    EXPECT_EQ(got->detected_at_ms, 0);
}

// 场景:非法 region 字符串 → 写入被拒绝,文件保持空,读 nullopt
TEST_F(StateJsonWebSearchTest, InvalidRegionRejected) {
    acecode::WebSearchRegionCache c;
    c.region = "asia";
    c.detected_at_ms = 123;
    acecode::write_web_search_region_cache(c);

    EXPECT_FALSE(acecode::read_web_search_region_cache().has_value());
}

// 场景:已有 unrelated 顶层 key 时,写 web_search 不破坏其他 key
TEST_F(StateJsonWebSearchTest, WritePreservesOtherKeys) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);

    acecode::WebSearchRegionCache c;
    c.region = "cn";
    c.detected_at_ms = 42;
    acecode::write_web_search_region_cache(c);

    // 同时读两边都能拿到
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
    auto cache = acecode::read_web_search_region_cache();
    ASSERT_TRUE(cache.has_value());
    EXPECT_EQ(cache->region, "cn");

    // raw JSON 里也能看到
    auto raw = read_raw();
    auto j = nlohmann::json::parse(raw);
    EXPECT_TRUE(j.contains("legacy_terminal_hint_shown"));
    EXPECT_TRUE(j.contains("web_search"));
}

// 场景:clear 后读 nullopt,但其他 key 不被影响
TEST_F(StateJsonWebSearchTest, ClearRemovesWebSearchOnly) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    acecode::WebSearchRegionCache c;
    c.region = "global";
    c.detected_at_ms = 99;
    acecode::write_web_search_region_cache(c);

    acecode::clear_web_search_region_cache();

    EXPECT_FALSE(acecode::read_web_search_region_cache().has_value());
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:存在但 region_detected 不是合法值 → nullopt(防止外部手编错值)
TEST_F(StateJsonWebSearchTest, IllegalRegionInFileRejectedOnRead) {
    nlohmann::json j;
    j["web_search"] = {{"region_detected", "asia"}, {"region_detected_at_ms", 5}};
    std::ofstream(path_) << j.dump();

    EXPECT_FALSE(acecode::read_web_search_region_cache().has_value());
}

// 场景:JSON 文件损坏 → read 返回 nullopt(不抛异常)
TEST_F(StateJsonWebSearchTest, CorruptedFileReturnsNullopt) {
    std::ofstream(path_) << "{invalid json {";
    EXPECT_FALSE(acecode::read_web_search_region_cache().has_value());
}

// 场景:detected_at_ms 缺失但 region 合法 → 仍接受,detected_at_ms = 0
TEST_F(StateJsonWebSearchTest, MissingTimestampDefaultsToZero) {
    nlohmann::json j;
    j["web_search"] = {{"region_detected", "global"}};
    std::ofstream(path_) << j.dump();

    auto got = acecode::read_web_search_region_cache();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->region, "global");
    EXPECT_EQ(got->detected_at_ms, 0);
}
