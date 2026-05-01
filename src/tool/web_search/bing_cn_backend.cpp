#include "bing_cn_backend.hpp"

#include "html_utils.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/base64.hpp"
#include "utils/logger.hpp"

#include <cpr/cpr.h>

#include <chrono>
#include <string>

namespace acecode::web_search {

namespace {

constexpr const char* kEndpoint = "https://cn.bing.com/search";
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0";
constexpr std::size_t kSnippetMaxCps = 200;

std::string url_encode(std::string_view in) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// 在 [hint, block_end) 区间找形如 `attr="value"` 的属性值,解码 entity 后填入 out。
bool extract_attr_value(std::string_view html, std::size_t hint,
                        std::size_t block_end, std::string_view attr,
                        std::string& out) {
    std::string needle(attr);
    needle += "=\"";
    std::size_t k = html.find(needle, hint);
    if (k == std::string_view::npos || k >= block_end) return false;
    std::size_t v_start = k + needle.size();
    std::size_t v_end = html.find('"', v_start);
    if (v_end == std::string_view::npos || v_end > block_end) return false;
    out = html_decode_entities(html.substr(v_start, v_end - v_start));
    return true;
}

// 提取 [pos, block_end) 中下一个 '>' 后到 '<' 前的内容,剥嵌套 tag 并 collapse。
bool extract_inner_text(std::string_view html, std::size_t pos,
                        std::size_t block_end, std::string& out) {
    std::size_t open = html.find('>', pos);
    if (open == std::string_view::npos || open > block_end) return false;
    // 跳过紧邻的可能的换行
    std::size_t i = open + 1;
    std::string raw;
    bool in_tag = false;
    while (i < block_end) {
        if (html[i] == '<') {
            // 看一下是不是 closing tag,是的话停;否则继续把内部 tag 当作 strip 对象
            // 简化:遇到任意 '<' 就开始 in_tag,'>' 关掉。需要支持 <b>...</b> 高亮。
            in_tag = true;
        } else if (html[i] == '>') {
            in_tag = false;
        } else if (!in_tag) {
            raw.push_back(html[i]);
        }
        ++i;
    }
    out = collapse_whitespace(html_decode_entities(raw));
    return true;
}

// 提取 <h2> 块内的第一个 <a href="..."> 文本与 href。
bool extract_h2_anchor(std::string_view html, std::size_t pos,
                       std::size_t block_end,
                       std::string& url_out, std::string& title_out) {
    std::size_t h2 = html.find("<h2", pos);
    if (h2 == std::string_view::npos || h2 >= block_end) return false;
    std::size_t a = html.find("<a ", h2);
    if (a == std::string_view::npos || a >= block_end) return false;

    std::string href;
    if (!extract_attr_value(html, a, block_end, "href", href)) return false;

    // a 内部文本
    std::size_t a_close = html.find('>', a);
    if (a_close == std::string_view::npos || a_close > block_end) return false;
    std::size_t a_end = html.find("</a>", a_close);
    if (a_end == std::string_view::npos || a_end > block_end) a_end = block_end;
    std::string raw_title;
    {
        bool in_tag = false;
        for (std::size_t i = a_close + 1; i < a_end; ++i) {
            if (html[i] == '<') in_tag = true;
            else if (html[i] == '>') in_tag = false;
            else if (!in_tag) raw_title.push_back(html[i]);
        }
    }
    title_out = collapse_whitespace(html_decode_entities(raw_title));
    url_out = decode_bing_redirect(href);
    return true;
}

// 提取 b_algo 内的 snippet —— 优先找 b_lineclamp4,fallback 到第一个 <p>。
std::string extract_bing_snippet(std::string_view html, std::size_t block_start,
                                 std::size_t block_end) {
    std::string snippet;
    constexpr std::string_view kClamp = "b_lineclamp";
    std::size_t s = html.find(kClamp, block_start);
    if (s != std::string_view::npos && s < block_end) {
        if (extract_inner_text(html.substr(0, block_end), s, block_end, snippet)) {
            return truncate_with_ellipsis(snippet, kSnippetMaxCps);
        }
    }
    std::size_t p = html.find("<p", block_start);
    if (p != std::string_view::npos && p < block_end) {
        std::size_t p_close = html.find("</p>", p);
        if (p_close == std::string_view::npos || p_close > block_end) p_close = block_end;
        if (extract_inner_text(html.substr(0, p_close), p, p_close, snippet)) {
            return truncate_with_ellipsis(snippet, kSnippetMaxCps);
        }
    }
    return {};
}

} // namespace

std::string decode_bing_redirect(std::string_view href) {
    if (href.find("bing.com/ck/") == std::string_view::npos) {
        return std::string(href);
    }
    // 找 query string 里的 u= 参数(最后一个 ? 之后)
    std::size_t q = href.find('?');
    if (q == std::string_view::npos) return std::string(href);
    std::size_t scan = q + 1;
    while (scan < href.size()) {
        std::size_t eq = href.find('=', scan);
        std::size_t amp = href.find('&', scan);
        if (eq == std::string_view::npos || (amp != std::string_view::npos && amp < eq)) {
            scan = (amp == std::string_view::npos) ? href.size() : amp + 1;
            continue;
        }
        std::string key(href.substr(scan, eq - scan));
        std::size_t v_end = (amp == std::string_view::npos) ? href.size() : amp;
        std::string val(href.substr(eq + 1, v_end - (eq + 1)));
        if (key == "u") {
            // Bing 的 u 参数有 "a1" 前缀(标识 base64 编码 URL)
            std::string payload = (val.rfind("a1", 0) == 0) ? val.substr(2) : val;
            auto decoded = acecode::base64_decode(payload);
            if (decoded.has_value()) {
                const std::string& s = *decoded;
                // decoded 应该像 http(s)://...,验证一下避免乱字节冒充 URL
                if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0) {
                    return s;
                }
            }
            // 解析失败 → 静默回退到原 href
            return std::string(href);
        }
        scan = (amp == std::string_view::npos) ? href.size() : amp + 1;
    }
    return std::string(href);
}

std::variant<std::vector<SearchHit>, SearchError>
parse_bing_cn_html(std::string_view html, int limit) {
    constexpr std::string_view kAnchor = "class=\"b_algo\"";
    if (html.find(kAnchor) == std::string_view::npos) {
        return SearchError{SearchError::Kind::Parse,
                           "Bing CN HTML structure changed (no b_algo block)",
                           "bing_cn"};
    }
    std::vector<SearchHit> hits;
    std::size_t pos = 0;
    while (hits.size() < static_cast<std::size_t>(limit)) {
        std::size_t a = html.find(kAnchor, pos);
        if (a == std::string_view::npos) break;
        std::size_t next_a = html.find(kAnchor, a + kAnchor.size());
        std::size_t block_end = (next_a == std::string_view::npos) ? html.size() : next_a;

        SearchHit h;
        if (!extract_h2_anchor(html, a, block_end, h.url, h.title)) {
            pos = block_end;
            continue;
        }
        h.snippet = extract_bing_snippet(html, a, block_end);

        if (!h.url.empty() && !h.title.empty()) {
            hits.push_back(std::move(h));
        }
        pos = block_end;
    }
    if (hits.empty()) {
        return SearchError{SearchError::Kind::Parse,
                           "Bing CN returned 0 parseable results",
                           "bing_cn"};
    }
    return hits;
}

namespace {

HttpProbeResult cpr_bing_get(const std::string& url, int timeout_ms) {
    HttpProbeResult r;
    auto proxy_opts = network::proxy_options_for(url);
    cpr::Header headers = {
        {"User-Agent", kUserAgent},
        {"Accept", "text/html,application/xhtml+xml"},
        {"Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8"},
    };
    cpr::Response resp = cpr::Get(
        cpr::Url{url},
        headers,
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{timeout_ms}
    );
    r.status_code = resp.status_code;
    r.body = resp.text;
    if (resp.status_code == 0) r.error_message = resp.error.message;
    return r;
}

} // namespace

BingCnBackend::BingCnBackend(int timeout_ms, HttpFetchFn fetch)
    : timeout_ms_(timeout_ms), fetch_(fetch ? std::move(fetch) : cpr_bing_get) {}

std::variant<SearchResponse, SearchError>
BingCnBackend::search(std::string_view query, int limit,
                      const std::atomic<bool>* abort) {
    if (abort && abort->load()) {
        return SearchError{SearchError::Kind::Network, "aborted", name()};
    }
    auto t_start = std::chrono::steady_clock::now();
    std::string url = std::string(kEndpoint) + "?q=" + url_encode(query);

    HttpProbeResult resp = fetch_(url, timeout_ms_);
    if (abort && abort->load()) {
        return SearchError{SearchError::Kind::Network, "aborted", name()};
    }

    if (resp.status_code == 0) {
        return SearchError{SearchError::Kind::Network,
                           "cn.bing.com unreachable: " + resp.error_message,
                           name()};
    }
    if (resp.status_code == 429) {
        return SearchError{SearchError::Kind::RateLimited,
                           "Bing CN returned HTTP 429 (rate limited)",
                           name()};
    }
    if (resp.status_code >= 500 && resp.status_code < 600) {
        return SearchError{SearchError::Kind::Network,
                           "Bing CN returned HTTP " +
                               std::to_string(resp.status_code),
                           name()};
    }
    if (resp.status_code != 200) {
        return SearchError{SearchError::Kind::Network,
                           "Bing CN returned unexpected HTTP " +
                               std::to_string(resp.status_code),
                           name()};
    }

    auto parsed = parse_bing_cn_html(resp.body, limit);
    if (std::holds_alternative<SearchError>(parsed)) {
        return std::get<SearchError>(parsed);
    }
    auto t_end = std::chrono::steady_clock::now();
    SearchResponse out;
    out.hits = std::move(std::get<std::vector<SearchHit>>(parsed));
    out.backend_name = name();
    out.duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count());
    return out;
}

} // namespace acecode::web_search
