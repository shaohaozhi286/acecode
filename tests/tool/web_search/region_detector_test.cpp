// 覆盖 src/tool/web_search/region_detector.{hpp,cpp}。
//
// 关键设计:RegionDetector 接受注入式 RegionProbeFn,这里用 mock 完全跳过
// 真实网络。同时把 state.json 路径切到测试临时目录,避免污染用户状态文件。
//
// 覆盖项:
//   - HEAD 200 → Global,缓存写入
//   - HEAD 301 → Global(3xx 也算可达)
//   - HEAD 超时(status=0)→ Cn
//   - HEAD DNS 失败(status=0 + 错误信息)→ Cn
//   - HEAD 405 + GET 200 → Global(自动回退)
//   - HEAD 405 + GET 也失败 → Cn
//   - get_or_detect:有缓存 → 不调 probe;无缓存 → 触发探测
//   - invalidate 后 get_or_detect 重新探测
//   - abort 触发返回 Unknown,不调 probe

#include <gtest/gtest.h>

#include "tool/web_search/region_detector.hpp"
#include "utils/state_file.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace acecode;
using namespace acecode::web_search;

namespace {

class RegionDetectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto tmp = fs::temp_directory_path() /
                   ("acecode_region_test_" + std::to_string(std::rand()));
        fs::create_directories(tmp);
        path_ = (tmp / "state.json").string();
        set_state_file_path_for_test(path_);
    }
    void TearDown() override {
        set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(fs::path(path_).parent_path(), ec);
    }
    std::string path_;
};

struct ProbeCall {
    std::string url;
    std::string method;
};

// 构造一个固定脚本响应的 probe:每次调用按顺序消耗一个 ProbeResult。
// 同时记录调用历史到 calls 中,便于断言"是否走了 GET fallback"等。
RegionProbeFn make_scripted_probe(std::vector<ProbeResult> script,
                                  std::vector<ProbeCall>& calls) {
    auto idx = std::make_shared<std::size_t>(0);
    auto sc = std::make_shared<std::vector<ProbeResult>>(std::move(script));
    return [sc, idx, &calls](const std::string& url, const std::string& method,
                              int) -> ProbeResult {
        calls.push_back({url, method});
        if (*idx >= sc->size()) {
            return ProbeResult{0, "scripted-exhausted"};
        }
        return (*sc)[(*idx)++];
    };
}

} // namespace

// 场景:HEAD 200 → Global,缓存被写入 state.json
TEST_F(RegionDetectorTest, Head200YieldsGlobal) {
    std::vector<ProbeCall> calls;
    RegionDetector d(2000, make_scripted_probe({{200, ""}}, calls));
    EXPECT_EQ(d.detect_now(), Region::Global);

    auto cache = read_web_search_region_cache();
    ASSERT_TRUE(cache.has_value());
    EXPECT_EQ(cache->region, "global");
    EXPECT_GT(cache->detected_at_ms, 0);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].method, "HEAD");
}

// 场景:HEAD 301 同样算可达 → Global(无需 follow redirect)
TEST_F(RegionDetectorTest, Head301YieldsGlobal) {
    std::vector<ProbeCall> calls;
    RegionDetector d(2000, make_scripted_probe({{301, ""}}, calls));
    EXPECT_EQ(d.detect_now(), Region::Global);
}

// 场景:HEAD 超时(status=0)→ Cn,缓存写入 cn
TEST_F(RegionDetectorTest, TimeoutYieldsCn) {
    std::vector<ProbeCall> calls;
    RegionDetector d(2000,
                     make_scripted_probe({{0, "operation timed out"}}, calls));
    EXPECT_EQ(d.detect_now(), Region::Cn);

    auto cache = read_web_search_region_cache();
    ASSERT_TRUE(cache.has_value());
    EXPECT_EQ(cache->region, "cn");
}

// 场景:DNS 失败(status=0 + 错误信息)同样视为 Cn
TEST_F(RegionDetectorTest, DnsFailureYieldsCn) {
    std::vector<ProbeCall> calls;
    RegionDetector d(2000,
                     make_scripted_probe({{0, "could not resolve host"}}, calls));
    EXPECT_EQ(d.detect_now(), Region::Cn);
}

// 场景:HEAD 405 + GET 200 → Global,自动回退
TEST_F(RegionDetectorTest, Head405FallsBackToGet) {
    std::vector<ProbeCall> calls;
    RegionDetector d(
        2000, make_scripted_probe({{405, ""}, {200, ""}}, calls));
    EXPECT_EQ(d.detect_now(), Region::Global);

    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].method, "HEAD");
    EXPECT_EQ(calls[1].method, "GET");
}

// 场景:HEAD 405 + GET 超时 → Cn(两次都失败)
TEST_F(RegionDetectorTest, Head405GetAlsoFailsYieldsCn) {
    std::vector<ProbeCall> calls;
    RegionDetector d(
        2000, make_scripted_probe({{405, ""}, {0, "timeout"}}, calls));
    EXPECT_EQ(d.detect_now(), Region::Cn);
}

// 场景:get_or_detect 有缓存 → 不调 probe
TEST_F(RegionDetectorTest, GetOrDetectUsesCacheWhenAvailable) {
    WebSearchRegionCache c;
    c.region = "global";
    c.detected_at_ms = 12345;
    write_web_search_region_cache(c);

    std::vector<ProbeCall> calls;
    RegionDetector d(2000, make_scripted_probe({{0, "should-not-be-called"}}, calls));
    EXPECT_EQ(d.get_or_detect(), Region::Global);
    EXPECT_TRUE(calls.empty());
}

// 场景:get_or_detect 无缓存 → 触发探测,结果落盘
TEST_F(RegionDetectorTest, GetOrDetectTriggersProbeWhenNoCache) {
    std::vector<ProbeCall> calls;
    RegionDetector d(2000, make_scripted_probe({{200, ""}}, calls));
    EXPECT_EQ(d.get_or_detect(), Region::Global);
    EXPECT_EQ(calls.size(), 1u);
    EXPECT_TRUE(read_web_search_region_cache().has_value());
}

// 场景:invalidate 后 get_or_detect 重新探测,结果可能与之前不同
TEST_F(RegionDetectorTest, InvalidateForcesReProbe) {
    WebSearchRegionCache c;
    c.region = "global";
    c.detected_at_ms = 1;
    write_web_search_region_cache(c);

    std::vector<ProbeCall> calls;
    RegionDetector d(2000, make_scripted_probe({{0, "now-blocked"}}, calls));

    // 先用缓存
    EXPECT_EQ(d.get_or_detect(), Region::Global);
    EXPECT_TRUE(calls.empty());

    d.invalidate();

    // invalidate 后再调 → 触发探测,结果是 Cn
    EXPECT_EQ(d.get_or_detect(), Region::Cn);
    EXPECT_EQ(calls.size(), 1u);
    auto cache = read_web_search_region_cache();
    ASSERT_TRUE(cache.has_value());
    EXPECT_EQ(cache->region, "cn");
}

// 场景:abort 在调 probe 之前置 true → 返回 Unknown,不调 probe,不写缓存
TEST_F(RegionDetectorTest, AbortBeforeProbeReturnsUnknown) {
    std::vector<ProbeCall> calls;
    RegionDetector d(2000, make_scripted_probe({{200, ""}}, calls));

    std::atomic<bool> abort{true};
    EXPECT_EQ(d.detect_now(&abort), Region::Unknown);
    EXPECT_TRUE(calls.empty());
    EXPECT_FALSE(read_web_search_region_cache().has_value());
}

// 场景:cached_region 在没有缓存时返回 Unknown
TEST_F(RegionDetectorTest, CachedRegionWithoutCacheReturnsUnknown) {
    RegionDetector d(2000, make_scripted_probe({}, *new std::vector<ProbeCall>{}));
    EXPECT_EQ(d.cached_region(), Region::Unknown);
}
