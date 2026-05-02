#pragma once

// Backend 路由器:管理已注册的 backend、根据 cfg + region 决定 active backend、
// 在 Network 错误时自动 fallback 到对侧。线程安全(内部互斥锁)。
//
// 详见 openspec/changes/add-web-search-tool/design.md Decision 3-4。

#include "backend.hpp"
#include "region_detector.hpp"
#include "config/config.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>

namespace acecode::web_search {

// notify 用于把"Warning: Switched to bing_cn"等信息回传给上层(TUI / 工具流)。
// notify 可以为空(测试 / daemon 不关心可以传 nullptr)。
using NotifyFn = std::function<void(const std::string& yellow_message)>;

class BackendRouter {
public:
    // cfg 引用要在 router 生命周期内保持有效(通常 cfg 是进程级 AppConfig)。
    explicit BackendRouter(const WebSearchConfig& cfg);

    // 注册一个 backend(name 即 backend->name())。生产代码通常在构造后立即
    // 调 register_default_backends(...) 一次性注入 DDG + Bing CN。
    void register_backend(std::unique_ptr<WebSearchBackend> b);

    // 根据 cfg + region 选定 active backend。region == Unknown 时悲观选 bing_cn。
    // 有效 backend 名:auto / duckduckgo / bing_cn / bochaai / tavily;后两个
    // 当前未注册时 LOG_WARN 并 fallback 到 auto。
    void resolve_active(Region region);

    // 显式切 active backend(/websearch use)— 不持久化。未知 / 未注册的
    // backend 名返回 false 并保持当前不变。
    bool set_active(const std::string& name);

    // 重置回 cfg 声明(等价 resolve_active(cached_region))。
    void reset_to_config(Region region);

    // 当前 active backend 名(线程安全)。无 active 时返回空字符串。
    std::string active_name() const;

    // 一次搜索 + 失败 fallback。Network 错误 → 试对侧;成功后 set_active +
    // 更新 region 缓存 + notify 回调。Parse / RateLimited / Disabled 直接返回。
    std::variant<SearchResponse, SearchError> search_with_fallback(
        std::string_view query,
        int limit,
        const std::atomic<bool>* abort,
        const NotifyFn& notify);

    // /websearch 命令显示用的状态快照。
    nlohmann::json status_snapshot(Region region) const;

    // 测试 hook:列出已注册的 backend 名(按 name 排序)。
    std::vector<std::string> registered_names_for_test() const;

private:
    // 计算 cfg + region 推导出的 active backend 名(纯函数,线程安全外取锁前调)。
    // 不访问 backends_;只看 cfg + region + 静态映射。
    std::string compute_active_name(Region region) const;

    // 取对侧的 backend 名(duckduckgo ↔ bing_cn)。其他 backend 名返回空。
    std::string opposite_of(const std::string& name) const;

    // 找 backend 实例,未找到返回 nullptr。需调用方持锁。
    WebSearchBackend* find_unlocked(const std::string& name);

    const WebSearchConfig& cfg_;
    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<WebSearchBackend>> backends_;
    std::string active_;
};

// 便利函数:创建 DDG + Bing CN backend 并注册到 router。bochaai/tavily 占位
// — 配置命中时 LOG_WARN 提示未实现,不抛出。
void register_default_backends(BackendRouter& router, const WebSearchConfig& cfg);

} // namespace acecode::web_search
