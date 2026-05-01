// 覆盖 src/tool/web_search/bing_cn_backend.{hpp,cpp}。
//
// 重点:
//   - parser 直接喂内嵌 fixture(避免文件 I/O,与 codebase 约定一致)
//   - decode_bing_redirect 单独测试 — 这是 Bing 特有的复杂逻辑,要密
//   - 解码失败 / 不像 URL 都要静默回退到原 href
//
// 覆盖项:
//   - 8 条 b_algo 块,部分含 ck/a 跳转,部分直链
//   - 跳转链 base64 解码到真实 URL
//   - 解码非 http(s) → 回退到原 href(防伪 URL 注入)
//   - base64 非法 → 回退到原 href(不阻塞)
//   - 缺锚点 → Parse 错误
//   - HTTP 5xx → Network,message 含 status

#include <gtest/gtest.h>

#include "tool/web_search/bing_cn_backend.hpp"

#include <atomic>
#include <string>

using namespace acecode;
using namespace acecode::web_search;

namespace {

// "https://example.com/path" 的 base64 = aHR0cHM6Ly9leGFtcGxlLmNvbS9wYXRo
const char* kFixtureBingNormal = R"HTML(<!DOCTYPE html>
<html><body>
<ol id="b_results">

<li class="b_algo">
  <h2><a href="https://www.rust-lang.org/" target="_blank">
    Rust Programming Language</a></h2>
  <div class="b_caption">
    <p>A language empowering everyone to build reliable software.</p>
  </div>
</li>

<li class="b_algo">
  <h2><a href="https://cn.bing.com/ck/a?!&amp;&amp;p=xyz&amp;u=a1aHR0cHM6Ly9leGFtcGxlLmNvbS9wYXRo&amp;ntb=1" target="_blank">
    Example via &amp; redirector</a></h2>
  <p class="b_lineclamp4 b_algoSlug">Decoded URL test.</p>
</li>

<li class="b_algo">
  <h2><a href="https://doc.rust-lang.org/book/" target="_blank">
    The Rust Book</a></h2>
  <p>The official guide.</p>
</li>

<li class="b_algo">
  <h2><a href="https://crates.io/" target="_blank">
    crates.io</a></h2>
  <p>Rust package registry.</p>
</li>

<li class="b_algo">
  <h2><a href="https://tokio.rs/" target="_blank">
    Tokio runtime</a></h2>
  <p>Async runtime for Rust applications.</p>
</li>

<li class="b_algo">
  <h2><a href="https://serde.rs/" target="_blank">
    Serde framework</a></h2>
  <p>Serialization framework.</p>
</li>

<li class="b_algo">
  <h2><a href="https://www.crates.io/keywords/web" target="_blank">
    Web crates</a></h2>
  <p>HTTP-related crates.</p>
</li>

<li class="b_algo">
  <h2><a href="https://lib.rs/" target="_blank">
    lib.rs alternative index</a></h2>
  <p>Curated Rust crate index.</p>
</li>

</ol>
</body></html>
)HTML";

const char* kFixtureBingNoAnchor = R"HTML(<!DOCTYPE html>
<html><body>
<div id="no_results">未找到相关结果</div>
</body></html>
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

// === decode_bing_redirect ===

// 场景:非 ck/a 链接原样返回(不动)
TEST(DecodeBingRedirect, NonRedirectorReturnsAsIs) {
    EXPECT_EQ(decode_bing_redirect("https://example.com/path"),
              "https://example.com/path");
    EXPECT_EQ(decode_bing_redirect(""), "");
}

// 场景:ck/a 中有 u=a1<base64>,正确解码成真实 URL
TEST(DecodeBingRedirect, ValidBase64DecodesToTargetUrl) {
    // u=a1aHR0cHM6Ly9leGFtcGxlLmNvbS9wYXRo → "https://example.com/path"
    auto out = decode_bing_redirect(
        "https://cn.bing.com/ck/a?!&p=xyz&u=a1aHR0cHM6Ly9leGFtcGxlLmNvbS9wYXRo&ntb=1");
    EXPECT_EQ(out, "https://example.com/path");
}

// 场景:base64 非法 → 静默回退到原 href(不抛异常)
TEST(DecodeBingRedirect, InvalidBase64FallsBackToOriginal) {
    std::string original = "https://cn.bing.com/ck/a?u=a1@@@@invalid_base64";
    EXPECT_EQ(decode_bing_redirect(original), original);
}

// 场景:解码出来的不是 http(s) → 回退(防 javascript:/data: 等伪 URL)
TEST(DecodeBingRedirect, NonHttpDecodedFallsBack) {
    // "javascript:alert(1)" 的 base64 = amF2YXNjcmlwdDphbGVydCgxKQ==
    std::string original =
        "https://cn.bing.com/ck/a?u=a1amF2YXNjcmlwdDphbGVydCgxKQ==";
    EXPECT_EQ(decode_bing_redirect(original), original);
}

// 场景:ck/a 链接但没有 u 参数 → 回退到原 href
TEST(DecodeBingRedirect, NoUParamFallsBack) {
    std::string original = "https://cn.bing.com/ck/a?p=xyz&ntb=1";
    EXPECT_EQ(decode_bing_redirect(original), original);
}

// === parser direct ===

// 场景:8 条 b_algo,正常解析前 5 条,跳转链已被解码
TEST(BingCnParser, FiveResultsWithRedirectorDecoded) {
    auto parsed = parse_bing_cn_html(kFixtureBingNormal, 5);
    ASSERT_TRUE(std::holds_alternative<std::vector<SearchHit>>(parsed));
    const auto& hits = std::get<std::vector<SearchHit>>(parsed);
    ASSERT_EQ(hits.size(), 5u);
    EXPECT_EQ(hits[0].url, "https://www.rust-lang.org/");
    EXPECT_EQ(hits[1].url, "https://example.com/path"); // 解码后的 URL
    EXPECT_EQ(hits[1].title, "Example via & redirector");
    EXPECT_EQ(hits[2].url, "https://doc.rust-lang.org/book/");
    // 任何 hit 的 url 都不应该再以 ck/a 开头
    for (const auto& h : hits) {
        EXPECT_EQ(h.url.find("/ck/"), std::string::npos)
            << "url unexpectedly still a redirector: " << h.url;
    }
}

// 场景:limit > 实际结果时取全部
TEST(BingCnParser, LimitLargerThanResultsReturnsAll) {
    auto parsed = parse_bing_cn_html(kFixtureBingNormal, 100);
    ASSERT_TRUE(std::holds_alternative<std::vector<SearchHit>>(parsed));
    EXPECT_EQ(std::get<std::vector<SearchHit>>(parsed).size(), 8u);
}

// 场景:无 b_algo 锚点 → Parse 错误
TEST(BingCnParser, MissingAnchorReturnsParseError) {
    auto parsed = parse_bing_cn_html(kFixtureBingNoAnchor, 5);
    ASSERT_TRUE(std::holds_alternative<SearchError>(parsed));
    const auto& err = std::get<SearchError>(parsed);
    EXPECT_EQ(err.kind, SearchError::Kind::Parse);
    EXPECT_EQ(err.backend_name, "bing_cn");
}

// === backend with injected HTTP ===

// 场景:正常 200 响应 → SearchResponse,backend_name 正确
TEST(BingCnBackend, SuccessfulSearch) {
    BingCnBackend backend(8000, make_canned_fetch(200, kFixtureBingNormal));
    auto out = backend.search("rust", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchResponse>(out));
    const auto& resp = std::get<SearchResponse>(out);
    EXPECT_EQ(resp.backend_name, "bing_cn");
    EXPECT_EQ(resp.hits.size(), 5u);
}

// 场景:HTTP 503 → Network 错误,message 含 "503"
TEST(BingCnBackend, Http503MappedToNetwork) {
    BingCnBackend backend(8000, make_canned_fetch(503, "", ""));
    auto out = backend.search("x", 5, nullptr);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    const auto& err = std::get<SearchError>(out);
    EXPECT_EQ(err.kind, SearchError::Kind::Network);
    EXPECT_NE(err.message.find("503"), std::string::npos);
}

// 场景:abort 即时返回
TEST(BingCnBackend, AbortReturnsImmediately) {
    bool called = false;
    auto fetch = [&called](const std::string&, int) {
        called = true;
        return HttpProbeResult{200, std::string(kFixtureBingNormal), ""};
    };
    BingCnBackend backend(8000, fetch);
    std::atomic<bool> abort{true};
    auto out = backend.search("x", 5, &abort);
    ASSERT_TRUE(std::holds_alternative<SearchError>(out));
    EXPECT_FALSE(called);
}
