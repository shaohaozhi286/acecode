#pragma once

// 必应中国(cn.bing.com)HTML 爬取 backend。国内免费方案,零配置,
// 反爬最宽松,Bing 中文版结果质量与英文版相当。
//
// 端点:GET https://cn.bing.com/search?q=<encoded>
// 解析:`class="b_algo"` 块切分,内含 `<h2><a>` 与 `<p>` / `class="b_lineclamp4"`。
// 跳转链处理:`bing.com/ck/a?...&u=a1<base64-url>` 自动 base64 解码到真实 URL。

#include "backend.hpp"
#include "duckduckgo_backend.hpp" // for HttpFetchFn / HttpProbeResult

#include <atomic>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace acecode::web_search {

class BingCnBackend : public WebSearchBackend {
public:
    explicit BingCnBackend(int timeout_ms, HttpFetchFn fetch = nullptr);

    std::string name() const override { return "bing_cn"; }
    bool requires_api_key() const override { return false; }

    std::variant<SearchResponse, SearchError> search(
        std::string_view query,
        int limit,
        const std::atomic<bool>* abort) override;

private:
    int timeout_ms_;
    HttpFetchFn fetch_;
};

// 解析必应跳转链 `https://(cn\.)?bing.com/ck/a?...&u=a1<base64>` 中的 u= 段
// (a1 前缀剥掉),base64-decode 出真实 URL。失败(无 u= / decode 失败 /
// decoded 不像 URL)→ 返回原 href。
std::string decode_bing_redirect(std::string_view href);

// 暴露 parser 给单测直接喂 fixture HTML。
std::variant<std::vector<SearchHit>, SearchError>
parse_bing_cn_html(std::string_view html, int limit);

} // namespace acecode::web_search
