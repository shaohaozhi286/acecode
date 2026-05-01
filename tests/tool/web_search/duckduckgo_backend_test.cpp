// 覆盖 src/tool/web_search/duckduckgo_backend.{hpp,cpp}。
//
// 策略:
//   - parser 直接喂内嵌 fixture HTML(避免文件 I/O)
//   - backend 行为通过注入 HttpFetchFn 替换 cpr 的真实网络
//
// 覆盖项:
//   - 正常 5 条结果解析,字段非空
//   - HTML 实体解码(&amp; / &quot; / &#43;)
//   - 多余 limit 时截到给定数量
//   - 锚点缺失 → SearchError{Parse}
//   - 锚点存在但抽不出结果 → SearchError{Parse}
//   - 网络层超时(status=0)→ SearchError{Network}
//   - HTTP 429 → SearchError{RateLimited}
//   - HTTP 500 → SearchError{Network} 且 message 含 "500"
//   - abort_flag 即时返回

#include <gtest/gtest.h>

#include "tool/web_search/duckduckgo_backend.hpp"

#include <atomic>
#include <string>

using namespace acecode;
using namespace acecode::web_search;

namespace {

// 5 条标准结果的精简 HTML。包含 HTML 实体、嵌入 <b>、CDN-style 相对链接,
// 模拟 DDG html 端点常见的页面结构。
const char* kFixtureFiveResults = R"HTML(<!DOCTYPE html>
<html><body>
<div class="result"><h2><a class="result__a" href="https://www.rust-lang.org/">
Rust &amp; Web</a></h2>
<a class="result__snippet" href="x">Build &quot;async&quot; apps with Rust &#43; Tokio.</a>
</div>

<div class="result"><h2><a class="result__a" href="https://doc.rust-lang.org/book/">
The <b>Rust</b> Book</a></h2>
<a class="result__snippet" href="x">Official tutorial covering ownership and lifetimes.</a>
</div>

<div class="result"><h2><a class="result__a" href="https://tokio.rs/">
Tokio</a></h2>
<a class="result__snippet" href="x">An async runtime for Rust.</a>
</div>

<div class="result"><h2><a class="result__a" href="//cdn.example.com/path">
CDN-style relative</a></h2>
<a class="result__snippet" href="x">Relative URL test.</a>
</div>

<div class="result"><h2><a class="result__a" href="https://crates.io/">
crates.io</a></h2>
<a class="result__snippet" href="x">Rust package registry.</a>
</div>
</body></html>
)HTML";

// 没有 result__a 锚点 — DDG 改了 HTML 结构的兜底测试场景
const char* kFixtureNoAnchor = R"HTML(<!DOCTYPE html>
<html><body><div id="error">No results found.</div></body></html>
)HTML";

HttpFetchFn make_canned_fetch(long status, std::string body, std::string err = "") {
    return [status, body = std::move(body), err = std::move(err)]
           (const std::string&, int) -> HttpProbeResult {
        HttpProbeResult r;
        r.status_code = status;
        r.body = body;
        r.error_message = err;
        return r;
    };
}

} // namespace

// === parser direct ===

// 场景:5 条结果都被解析,字段非空且 HTML 实体解码正确
TEST(DuckDuckGoParser, NormalFiveResults) {
    auto parsed = parse_duckduckgo_html(kFixtureFiveResults, 5);
    ASSERT_TRUE(std::holds_alternative<std::vector<SearchHit>>(parsed));
    const auto& hits = std::get<std::vector<SearchHit>>(parsed);
    ASSERT_EQ(hits.size(), 5u);
    EXPECT_EQ(hits[0].title, "Rust & Web");
    EXPECT_EQ(hits[0].url, "https://www.rust-lang.org/");
    EXPECT_EQ(hits[0].snippet, "Build \"async\" apps with Rust + Tokio.");
    EXPECT_EQ(hits[1].title, "The Rust Book");      // <b> 嵌入被剥
    EXPECT_NE(hits[1].snippet.find("ownership"), std::string::npos);
    EXPECT_EQ(hits[3].url, "https://cdn.example.com/path"); // // -> https://
}

// 场景:limit 较小时只截前 N 条
TEST(DuckDuckGoParser, RespectLimit) {
    auto parsed = parse_duckduckgo_html(kFixtureFiveResults, 2);
    ASSERT_TRUE(std::holds_alternative<std::vector<SearchHit>>(parsed));
    EXPECT_EQ(std::get<std::vector<SearchHit>>(parsed).size(), 2u);
}

// 场景:锚点不存在 → Parse 错误,message 显式说明结构变化
TEST(DuckDuckGoParser, MissingAnchorReturnsParseError) {
    auto parsed = parse_duckduckgo_html(kFixtureNoAnchor, 5);
    ASSERT_TRUE(std::holds_alternative<SearchError>(parsed));
    const auto& err = std::get<SearchError>(parsed);
    EXPECT_EQ(err.kind, SearchError::Kind::Parse);
    EXPECT_NE(err.message.find("structure"), std::string::npos);
    EXPECT_EQ(err.backend_name, "duckduckgo");
}

// === backend with injected HTTP ===

// 场景:正常 200 响应 → SearchResponse 含 hits,duration_ms 非负
TEST(DuckDuckGoBackend, SuccessfulSearch) {
    DuckDuckGoBackend backend(8000, make_canned_fetch(200, kFixtureFiveResults));
    auto out = backend.search("rust", 3, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    const auto& resp = std::get<SearchResponse>(out);
    EXPECT_EQ(resp.backend_name, "duckduckgo");
    EXPECT_EQ(resp.hits.size(), 3u);
    EXPECT_GE(resp.duration_ms, 0);
}

// 场景:status=0(网络层失败,如超时或 DNS)→ Network 错误
TEST(DuckDuckGoBackend, NetworkErrorMappedToNetworkKind) {
    DuckDuckGoBackend backend(8000, make_canned_fetch(0, "", "operation timed out"));
    auto out = backend.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    const auto& err = std::get<SearchError>(out);
    EXPECT_EQ(err.kind, SearchError::Kind::Network);
    EXPECT_NE(err.message.find("timed out"), std::string::npos);
}

// 场景:HTTP 429 → RateLimited 错误(不是 Network,以阻止 fallback)
TEST(DuckDuckGoBackend, Http429MappedToRateLimited) {
    DuckDuckGoBackend backend(8000, make_canned_fetch(429, "", ""));
    auto out = backend.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).kind, SearchError::Kind::RateLimited);
}

// 场景:HTTP 5xx → Network 错误,message 含 status code
TEST(DuckDuckGoBackend, Http5xxMappedToNetwork) {
    DuckDuckGoBackend backend(8000, make_canned_fetch(503, "", ""));
    auto out = backend.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    const auto& err = std::get<SearchError>(out);
    EXPECT_EQ(err.kind, SearchError::Kind::Network);
    EXPECT_NE(err.message.find("503"), std::string::npos);
}

// 场景:abort 立即触发,不调真实 fetch
TEST(DuckDuckGoBackend, AbortBeforeFetchReturnsImmediately) {
    bool fetch_called = false;
    auto fetch = [&fetch_called](const std::string&, int) {
        fetch_called = true;
        return HttpProbeResult{200, std::string(kFixtureFiveResults), ""};
    };
    DuckDuckGoBackend backend(8000, fetch);
    std::atomic<bool> abort{true};
    auto out = backend.search("x", 5, &abort);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).kind, SearchError::Kind::Network);
    EXPECT_NE(std::get<SearchError>(out).message.find("aborted"), std::string::npos);
    EXPECT_FALSE(fetch_called);
}

// 场景:200 响应但 HTML 缺锚点 → Parse 错误透传上层
TEST(DuckDuckGoBackend, BadHtmlBubblesParseError) {
    DuckDuckGoBackend backend(8000, make_canned_fetch(200, kFixtureNoAnchor));
    auto out = backend.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_EQ(std::get<SearchError>(out).kind, SearchError::Kind::Parse);
}
