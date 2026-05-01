// 覆盖 src/tool/web_search/web_search_tool.{hpp,cpp}。
//
// 重点是工具层的:
//   - JSON 入参解析(query 必选,limit 可选)
//   - query 为空 → SearchError{Disabled},不走 router
//   - limit 超过配置 max_results → 静默 clamp
//   - markdown 输出格式:标题 / URL / snippet 折行
//   - 失败文本格式
//   - ToolSummary 字段(成功 metrics 含 results/backend/time;失败含 failed/err)
//
// 通过 mock backend(已在 backend_router_test 验证过 router 行为)注入受控
// 响应,避开真实网络。

#include <gtest/gtest.h>

#include "tool/web_search/web_search_tool.hpp"
#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "config/config.hpp"
#include "utils/state_file.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace fs = std::filesystem;

using namespace acecode;
using namespace acecode::web_search;

namespace {

class MockBackend : public WebSearchBackend {
public:
    MockBackend(std::string n, std::variant<SearchResponse, SearchError> r)
        : name_(std::move(n)), resp_(std::move(r)) {}
    std::string name() const override { return name_; }
    bool requires_api_key() const override { return false; }
    std::variant<SearchResponse, SearchError>
    search(std::string_view, int limit_in,
           const std::atomic<bool>*) override {
        last_limit = limit_in;
        return resp_;
    }
    std::string name_;
    std::variant<SearchResponse, SearchError> resp_;
    int last_limit = -1;
};

class WebSearchToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto tmp = fs::temp_directory_path() /
                   ("acecode_ws_tool_test_" + std::to_string(std::rand()));
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

SearchResponse make_resp(const std::string& backend, std::size_t n) {
    SearchResponse r;
    r.backend_name = backend;
    r.duration_ms = 1230;
    for (std::size_t i = 0; i < n; ++i) {
        r.hits.push_back({"title-" + std::to_string(i),
                          "https://example.com/" + std::to_string(i),
                          "snippet line " + std::to_string(i)});
    }
    return r;
}

} // namespace

// 场景:成功调用 → markdown 文本 + ToolSummary metrics
TEST_F(WebSearchToolTest, SuccessfulCallProducesMarkdownAndSummary) {
    WebSearchConfig cfg;
    BackendRouter router(cfg);
    auto mock = std::make_unique<MockBackend>("duckduckgo", make_resp("duckduckgo", 3));
    router.register_backend(std::move(mock));
    router.register_backend(std::make_unique<MockBackend>(
        "bing_cn", SearchError{SearchError::Kind::Network, "n/a", "bing_cn"}));
    router.resolve_active(Region::Global);

    auto tool = create_web_search_tool(router, cfg);
    ToolContext ctx;
    auto r = tool.execute(R"({"query": "rust async"})", ctx);

    EXPECT_TRUE(r.success);
    EXPECT_NE(r.output.find("Web search results for \"rust async\""), std::string::npos);
    EXPECT_NE(r.output.find("(duckduckgo, 3 results)"), std::string::npos);
    EXPECT_NE(r.output.find("**title-0**"), std::string::npos);
    EXPECT_NE(r.output.find("https://example.com/0"), std::string::npos);

    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "web_search");
    EXPECT_EQ(r.summary->object, "\"rust async\"");
    bool found_results = false, found_backend = false;
    for (const auto& [k, v] : r.summary->metrics) {
        if (k == "results") { EXPECT_EQ(v, "3"); found_results = true; }
        if (k == "backend") { EXPECT_EQ(v, "duckduckgo"); found_backend = true; }
    }
    EXPECT_TRUE(found_results);
    EXPECT_TRUE(found_backend);
}

// 场景:query 缺失 / 空串 → Disabled 错误,router 不被调用
TEST_F(WebSearchToolTest, EmptyQueryRejectedBeforeRouter) {
    WebSearchConfig cfg;
    BackendRouter router(cfg);
    auto mock = std::make_unique<MockBackend>("duckduckgo", make_resp("duckduckgo", 1));
    auto* mock_raw = mock.get();
    router.register_backend(std::move(mock));
    router.register_backend(std::make_unique<MockBackend>(
        "bing_cn", make_resp("bing_cn", 1)));
    router.resolve_active(Region::Global);

    auto tool = create_web_search_tool(router, cfg);
    ToolContext ctx;
    auto r = tool.execute(R"({"query": ""})", ctx);
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("query is required"), std::string::npos);
    EXPECT_EQ(mock_raw->last_limit, -1); // 未调
}

// 场景:limit 超过配置 max_results → clamp 到 cfg.max_results
TEST_F(WebSearchToolTest, LimitClampedToConfigMaxResults) {
    WebSearchConfig cfg;
    cfg.max_results = 3;
    BackendRouter router(cfg);
    auto mock = std::make_unique<MockBackend>("duckduckgo", make_resp("duckduckgo", 3));
    auto* mock_raw = mock.get();
    router.register_backend(std::move(mock));
    router.register_backend(std::make_unique<MockBackend>(
        "bing_cn", make_resp("bing_cn", 1)));
    router.resolve_active(Region::Global);

    auto tool = create_web_search_tool(router, cfg);
    ToolContext ctx;
    auto r = tool.execute(R"({"query": "x", "limit": 50})", ctx);
    EXPECT_TRUE(r.success);
    EXPECT_EQ(mock_raw->last_limit, 3); // clamp 到 cfg.max_results
}

// 场景:limit 超过 hard cap 10 也被 clamp(即使 cfg.max_results 允许更大)
TEST_F(WebSearchToolTest, LimitHardCappedAt10) {
    WebSearchConfig cfg;
    cfg.max_results = 10; // 上限 10(load_config 已校验)
    BackendRouter router(cfg);
    auto mock = std::make_unique<MockBackend>("duckduckgo", make_resp("duckduckgo", 5));
    auto* mock_raw = mock.get();
    router.register_backend(std::move(mock));
    router.register_backend(std::make_unique<MockBackend>(
        "bing_cn", make_resp("bing_cn", 1)));
    router.resolve_active(Region::Global);

    auto tool = create_web_search_tool(router, cfg);
    ToolContext ctx;
    auto r = tool.execute(R"({"query": "x", "limit": 100})", ctx);
    EXPECT_TRUE(r.success);
    EXPECT_EQ(mock_raw->last_limit, 10);
}

// 场景:失败文本格式包含 kind / backend / message
TEST_F(WebSearchToolTest, ErrorTextFormat) {
    WebSearchConfig cfg;
    BackendRouter router(cfg);
    router.register_backend(std::make_unique<MockBackend>(
        "duckduckgo",
        SearchError{SearchError::Kind::Network, "ddg unreachable", "duckduckgo"}));
    router.register_backend(std::make_unique<MockBackend>(
        "bing_cn",
        SearchError{SearchError::Kind::Network, "bing unreachable", "bing_cn"}));
    router.resolve_active(Region::Global);

    auto tool = create_web_search_tool(router, cfg);
    ToolContext ctx;
    auto r = tool.execute(R"({"query": "x"})", ctx);
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("Web search failed"), std::string::npos);
    EXPECT_NE(r.output.find("Network"), std::string::npos);
    EXPECT_NE(r.output.find("bing_cn"), std::string::npos); // 双 fail 时返回 fallback 错误
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "web_search");
}

// 场景:format_results_markdown 中 snippet 含 newline 被折叠成空格
TEST(WebSearchFormat, SnippetNewlineCollapsed) {
    SearchResponse resp;
    resp.backend_name = "duckduckgo";
    resp.hits.push_back({"T", "https://x", "Line one.\nLine two."});
    auto out = format_results_markdown("q", resp);
    EXPECT_NE(out.find("Line one. Line two."), std::string::npos);
    EXPECT_EQ(out.find("Line one.\nLine two."), std::string::npos);
}

// 场景:format_error_text 在有 fallback_err 时输出两行
TEST(WebSearchFormat, ErrorTextWithFallback) {
    SearchError primary{SearchError::Kind::Network, "ddg-fail", "duckduckgo"};
    SearchError fb{SearchError::Kind::Network, "bing-fail", "bing_cn"};
    auto out = format_error_text(primary, &fb);
    EXPECT_NE(out.find("Web search failed: Network error from duckduckgo: ddg-fail"),
              std::string::npos);
    EXPECT_NE(out.find("Tried fallback bing_cn: bing-fail"), std::string::npos);
}
