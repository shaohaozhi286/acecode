#include "duckduckgo_backend.hpp"

#include "html_utils.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/logger.hpp"

#include <cpr/cpr.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace acecode::web_search {

namespace {

constexpr const char* kEndpoint = "https://html.duckduckgo.com/html/";
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
constexpr std::size_t kSnippetMaxCps = 200;

// URL-encode for the query parameter. Only non-alphanumeric reserved chars
// are escaped — keeps the resulting URL readable in logs.
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

// Find the first occurrence of needle starting at pos. Returns string::npos.
std::size_t find_at(std::string_view hay, std::string_view needle, std::size_t pos) {
    return hay.find(needle, pos);
}

// Extract attribute value: looks for `attr="..."` starting at hint, returns
// the unescaped value. Caller passes an upper-bound (block_end) to avoid
// crossing block boundary on malformed input.
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

// Extract inner text after pos, stripping any nested tags. Used for DDG's
// <a class="result__a">...<b>highlight</b>...</a> structure where the title
// may contain HTML emphasis. close_tag identifies where to stop scanning
// (e.g. "</a>" for the link text, "</a>" for the snippet wrapped in <a>).
bool extract_inner_text_until(std::string_view html, std::size_t pos,
                              std::size_t block_end,
                              std::string_view close_tag, std::string& out) {
    std::size_t tag_close = html.find('>', pos);
    if (tag_close == std::string_view::npos || tag_close > block_end) return false;
    std::size_t end = html.find(close_tag, tag_close + 1);
    if (end == std::string_view::npos || end > block_end) end = block_end;
    std::string stripped;
    stripped.reserve(end - tag_close);
    bool in_tag = false;
    for (std::size_t i = tag_close + 1; i < end; ++i) {
        char c = html[i];
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) stripped.push_back(c);
    }
    out = collapse_whitespace(html_decode_entities(stripped));
    return true;
}

} // namespace

HttpProbeResult cpr_http_get(const std::string& url, int timeout_ms) {
    HttpProbeResult r;
    auto proxy_opts = network::proxy_options_for(url);
    cpr::Header headers = {
        {"User-Agent", kUserAgent},
        {"Accept", "text/html,application/xhtml+xml"},
        {"Accept-Language", "en-US,en;q=0.9"},
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
    if (resp.status_code == 0) {
        r.error_message = resp.error.message;
    }
    return r;
}

std::variant<std::vector<SearchHit>, SearchError>
parse_duckduckgo_html(std::string_view html, int limit) {
    constexpr std::string_view kAnchor = "class=\"result__a\"";
    if (html.find(kAnchor) == std::string_view::npos) {
        return SearchError{SearchError::Kind::Parse,
                           "DuckDuckGo HTML structure changed (no result__a anchor)",
                           "duckduckgo"};
    }

    std::vector<SearchHit> hits;
    std::size_t pos = 0;
    while (hits.size() < static_cast<std::size_t>(limit)) {
        std::size_t a = html.find(kAnchor, pos);
        if (a == std::string_view::npos) break;

        // 块结束 = 下一个 result__a 或文档末尾
        std::size_t next_a = html.find(kAnchor, a + kAnchor.size());
        std::size_t block_end = (next_a == std::string_view::npos) ? html.size() : next_a;

        // 找到包含 result__a 的 <a> 标签起点(往回找 '<')
        std::size_t tag_start = html.rfind('<', a);
        if (tag_start == std::string_view::npos || tag_start >= block_end) {
            pos = a + kAnchor.size();
            continue;
        }

        SearchHit h;
        if (!extract_attr_value(html, tag_start, block_end, "href", h.url)) {
            pos = block_end;
            continue;
        }
        // DDG sometimes returns relative URLs starting with `//` (CDN-style).
        if (h.url.rfind("//", 0) == 0) h.url = "https:" + h.url;

        // 取 <a ...>title</a> 中的 title(支持 <b> 高亮嵌套)
        if (!extract_inner_text_until(html, tag_start, block_end, "</a>", h.title)) {
            pos = block_end;
            continue;
        }

        // snippet = result__snippet 块的内文(可选,DDG 把 snippet 也包在 <a> 里)
        constexpr std::string_view kSnippetAnchor = "class=\"result__snippet\"";
        std::size_t s = html.find(kSnippetAnchor, a + kAnchor.size());
        if (s != std::string_view::npos && s < block_end) {
            // 找到包含 snippet anchor 的 <a> 起点
            std::size_t snippet_tag_start = html.rfind('<', s);
            if (snippet_tag_start != std::string_view::npos &&
                snippet_tag_start < block_end) {
                std::string snippet;
                if (extract_inner_text_until(html, snippet_tag_start,
                                              block_end, "</a>", snippet)) {
                    h.snippet = truncate_with_ellipsis(snippet, kSnippetMaxCps);
                }
            }
        }

        if (!h.url.empty() && !h.title.empty()) {
            hits.push_back(std::move(h));
        }
        pos = block_end;
    }

    if (hits.empty()) {
        return SearchError{SearchError::Kind::Parse,
                           "DuckDuckGo returned 0 parseable results",
                           "duckduckgo"};
    }
    return hits;
}

DuckDuckGoBackend::DuckDuckGoBackend(int timeout_ms, HttpFetchFn fetch)
    : timeout_ms_(timeout_ms), fetch_(fetch ? std::move(fetch) : cpr_http_get) {}

std::variant<SearchResponse, SearchError>
DuckDuckGoBackend::search(std::string_view query, int limit,
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
                           "duckduckgo.com unreachable: " + resp.error_message,
                           name()};
    }
    if (resp.status_code == 429) {
        return SearchError{SearchError::Kind::RateLimited,
                           "DuckDuckGo returned HTTP 429 (rate limited)",
                           name()};
    }
    if (resp.status_code >= 500 && resp.status_code < 600) {
        return SearchError{SearchError::Kind::Network,
                           "DuckDuckGo returned HTTP " +
                               std::to_string(resp.status_code),
                           name()};
    }
    if (resp.status_code != 200) {
        return SearchError{SearchError::Kind::Network,
                           "DuckDuckGo returned unexpected HTTP " +
                               std::to_string(resp.status_code),
                           name()};
    }

    auto parsed = parse_duckduckgo_html(resp.body, limit);
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
