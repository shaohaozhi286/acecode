#pragma once

// 网络环境探测器:仅探测 duckduckgo.com 是否可达,推导 region。
// 详见 openspec/changes/add-web-search-tool/design.md Decision 1。
//
// 探测结果缓存到 ~/.acecode/state.json 的 web_search 段(state_file.hpp)。
// 永不自动过期 — 只有 /websearch refresh 或运行时 fallback 触发的
// invalidate() + detect_now() 会刷新。
//
// 注入式 HTTP 探针:生产代码默认走 cpr;测试可以注入 mock 跳过真实网络,
// 也可以为"HEAD 405 → GET 200"那种回退分支造可控输入。

#include "duckduckgo_backend.hpp" // for HttpFetchFn / HttpProbeResult

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace acecode::web_search {

enum class Region { Global, Cn, Unknown };

inline const char* region_str(Region r) {
    switch (r) {
        case Region::Global: return "global";
        case Region::Cn:     return "cn";
        case Region::Unknown:return "unknown";
    }
    return "unknown";
}

// HEAD/GET 探针。method 取 "HEAD" 或 "GET"。返回 status_code(0 = 网络层失败)。
// 默认实现 cpr_region_probe 在 region_detector.cpp 中。
struct ProbeResult {
    long status_code = 0;
    std::string error_message;
};
using RegionProbeFn = std::function<ProbeResult(const std::string& url,
                                                 const std::string& method,
                                                 int timeout_ms)>;

ProbeResult cpr_region_probe(const std::string& url,
                             const std::string& method,
                             int timeout_ms);

class RegionDetector {
public:
    // probe 缺省 = cpr_region_probe。timeout_ms 缺省 2000(2s)。
    explicit RegionDetector(int timeout_ms = 2000,
                            RegionProbeFn probe = nullptr);

    // 立即发探测请求并返回结果。HEAD 405 时回退到 GET。同时把成功结果写入 state.json。
    // abort 非空时,在请求前后各检查一次,中断返回 Region::Unknown。
    Region detect_now(const std::atomic<bool>* abort = nullptr);

    // 优先读 state.json 缓存;无缓存时调 detect_now。
    Region get_or_detect(const std::atomic<bool>* abort = nullptr);

    // 删除 state.json 中的缓存,下次 get_or_detect 触发实际探测。
    void invalidate();

    // 当前缓存(已读 state.json)。无缓存返回 Unknown。
    Region cached_region() const;
    long long cached_detected_at_ms() const;

    // 测试 hook:覆盖 timeout 与 probe(注入 mock)。
    void set_timeout_for_test(int ms) { timeout_ms_ = ms; }
    void set_probe_for_test(RegionProbeFn f) { probe_ = std::move(f); }

private:
    int timeout_ms_;
    RegionProbeFn probe_;
};

} // namespace acecode::web_search
