#pragma once

// DuckDuckGo HTML 爬取 backend。海外免费方案,零配置。
//
// 端点:GET https://html.duckduckgo.com/html/?q=<url-encoded-query>
// 解析:抓取 `class="result__a"`(链接 + 标题)+ `class="result__snippet"` 块。
//
// 与 Bing CN backend 配对组成本期的双 backend 自动适配方案,详见
// openspec/changes/add-web-search-tool/。

#include "backend.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace acecode::web_search {

// HTTP 探针注入接口。生产代码默认走 cpr;测试可注入 mock 跳过真实网络。
// 输入:url + 超时毫秒;输出:status_code + body + 网络层错误描述(空 = 无)。
struct HttpProbeResult {
    long status_code = 0;          // 0 = 网络失败(连接拒绝、DNS、超时)
    std::string body;
    std::string error_message;     // status_code == 0 时填,否则空
};
using HttpFetchFn =
    std::function<HttpProbeResult(const std::string& url, int timeout_ms)>;

// 默认基于 cpr 的 HTTP fetch 实现,内部走 ProxyResolver。供生产代码使用。
HttpProbeResult cpr_http_get(const std::string& url, int timeout_ms);

class DuckDuckGoBackend : public WebSearchBackend {
public:
    // timeout_ms 来自 cfg.web_search.timeout_ms。fetch 缺省 = cpr_http_get。
    explicit DuckDuckGoBackend(int timeout_ms, HttpFetchFn fetch = nullptr);

    std::string name() const override { return "duckduckgo"; }
    bool requires_api_key() const override { return false; }

    std::variant<SearchResponse, SearchError> search(
        std::string_view query,
        int limit,
        const std::atomic<bool>* abort) override;

private:
    int timeout_ms_;
    HttpFetchFn fetch_;
};

// 暴露内部 parser 供单测直接喂 fixture HTML。
// 失败 → SearchError{Parse, ...}(锚点不存在或抽不出任何结果)。
std::variant<std::vector<SearchHit>, SearchError>
parse_duckduckgo_html(std::string_view html, int limit);

} // namespace acecode::web_search
