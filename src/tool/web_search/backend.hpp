#pragma once

// 联网搜索 backend 抽象。所有搜索源(本期 DuckDuckGo / 必应中国 HTML 爬取,
// 后续 Tavily / 博查 API)都实现这个接口,让 BackendRouter 可统一驱动。
//
// 设计:
//   - search() 返回 std::variant<SearchResponse, SearchError>,与既有工具
//     ToolResult 风格(success bool + 可选错误)保持兼容,且强制调用方两路都处理
//   - abort 是非拥有指针,实现要 ≤100ms 内观察并返回(参考 bash_tool ctx.abort)
//   - SearchError::Kind 区分 Network / Parse / RateLimited / Disabled,让上层
//     的 BackendRouter 可以决定要不要 fallback(只在 Network 错误上 fallback)

#include <atomic>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace acecode {

struct SearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

struct SearchResponse {
    std::vector<SearchHit> hits;
    std::string backend_name;   // "duckduckgo" / "bing_cn" / ...
    int duration_ms = 0;        // 单次 search() 实际耗时
};

struct SearchError {
    // Network    — DNS/超时/连接拒绝/HTTP 5xx,会触发 BackendRouter 自动 fallback
    // Parse      — HTML 结构变化导致解析不出结果,fallback 也救不了,直接报告
    // RateLimited— HTTP 429 / 平台显式限流,告诉 LLM 别再重试
    // Disabled   — 输入非法(如 query 为空)/工具未启用,不发请求
    enum class Kind { Network, Parse, RateLimited, Disabled };
    Kind kind = Kind::Network;
    std::string message;
    std::string backend_name;   // 出错的 backend(便于 router 决策)
};

class WebSearchBackend {
public:
    virtual ~WebSearchBackend() = default;

    // backend 短名,与配置 / state.json / 命令使用的 token 保持一致。
    virtual std::string name() const = 0;

    // 是否需要 API key(用于启动期校验和 /websearch use 提示)。
    virtual bool requires_api_key() const = 0;

    // 同步执行一次搜索。
    //   query: 已 trim 过的非空查询串
    //   limit: 已 clamp 到 [1, 10] 的结果数量上限
    //   abort: 非拥有指针。非空时,实现需在不超过 100ms 内观察其变化并返回
    //          SearchError{Network, "aborted"};为空时不需要轮询。
    virtual std::variant<SearchResponse, SearchError> search(
        std::string_view query,
        int limit,
        const std::atomic<bool>* abort) = 0;
};

} // namespace acecode
