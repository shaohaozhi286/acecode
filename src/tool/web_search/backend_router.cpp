#include "backend_router.hpp"

#include "bing_cn_backend.hpp"
#include "duckduckgo_backend.hpp"
#include "utils/logger.hpp"
#include "utils/state_file.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace acecode::web_search {

namespace {

bool is_known_backend_name(const std::string& name) {
    return name == "duckduckgo" || name == "bing_cn" ||
           name == "bochaai" || name == "tavily";
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

BackendRouter::BackendRouter(const WebSearchConfig& cfg) : cfg_(cfg) {}

void BackendRouter::register_backend(std::unique_ptr<WebSearchBackend> b) {
    std::lock_guard<std::mutex> lk(mu_);
    if (b) {
        std::string name = b->name();
        backends_[std::move(name)] = std::move(b);
    }
}

std::string BackendRouter::compute_active_name(Region region) const {
    if (cfg_.backend == "auto") {
        if (region == Region::Global) return "duckduckgo";
        return "bing_cn"; // Cn 或 Unknown 都走悲观默认
    }
    if (cfg_.backend == "bochaai" || cfg_.backend == "tavily") {
        // 占位 backend 未实现 → fallback 到 auto
        if (region == Region::Global) return "duckduckgo";
        return "bing_cn";
    }
    return cfg_.backend; // duckduckgo / bing_cn(load_config 已校验)
}

void BackendRouter::resolve_active(Region region) {
    std::string desired = compute_active_name(region);
    std::lock_guard<std::mutex> lk(mu_);
    if (backends_.find(desired) == backends_.end()) {
        // 没注册(比如 cfg=tavily 但只注册了 ddg/bing_cn,且 compute 已 fallback,
        // 但又恰好 ddg/bing_cn 也没注册)→ 用任意一个已注册的兜底
        if (!backends_.empty()) {
            active_ = backends_.begin()->first;
            LOG_WARN("[web_search] desired backend '" + desired +
                     "' not registered; falling back to '" + active_ + "'");
        } else {
            active_.clear();
            LOG_WARN("[web_search] no backends registered, web_search disabled");
        }
        return;
    }
    active_ = std::move(desired);
}

bool BackendRouter::set_active(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (backends_.find(name) == backends_.end()) {
        return false;
    }
    active_ = name;
    return true;
}

void BackendRouter::reset_to_config(Region region) {
    resolve_active(region);
}

std::string BackendRouter::active_name() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

std::string BackendRouter::opposite_of(const std::string& name) const {
    if (name == "duckduckgo") return "bing_cn";
    if (name == "bing_cn") return "duckduckgo";
    return {};
}

WebSearchBackend* BackendRouter::find_unlocked(const std::string& name) {
    auto it = backends_.find(name);
    return (it == backends_.end()) ? nullptr : it->second.get();
}

std::variant<SearchResponse, SearchError>
BackendRouter::search_with_fallback(std::string_view query, int limit,
                                     const std::atomic<bool>* abort,
                                     const NotifyFn& notify) {
    // snapshot 当前 active + 拿到 backend 指针;mutex 持有时间最短。
    std::string primary;
    WebSearchBackend* primary_be = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        primary = active_;
        primary_be = find_unlocked(primary);
    }
    if (!primary_be) {
        return SearchError{SearchError::Kind::Disabled,
                           "no active web search backend",
                           ""};
    }

    auto first = primary_be->search(query, limit, abort);
    if (std::holds_alternative<SearchResponse>(first)) {
        return first;
    }
    const SearchError& err = std::get<SearchError>(first);
    if (err.kind != SearchError::Kind::Network) {
        // Parse / RateLimited / Disabled 不 fallback
        return first;
    }

    // Network → 试对侧
    std::string fallback_name = opposite_of(primary);
    WebSearchBackend* fallback_be = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fallback_be = fallback_name.empty() ? nullptr : find_unlocked(fallback_name);
    }
    if (!fallback_be) {
        return first; // 没对侧或对侧未注册,只能返回原错误
    }

    auto second = fallback_be->search(query, limit, abort);
    if (std::holds_alternative<SearchResponse>(second)) {
        // 切 active + 更新缓存 + notify
        {
            std::lock_guard<std::mutex> lk(mu_);
            active_ = fallback_name;
        }
        WebSearchRegionCache cache;
        // primary 失败 → 推断 region 与 fallback 对应:bing_cn ⇒ cn;duckduckgo ⇒ global
        cache.region = (fallback_name == "duckduckgo") ? "global" : "cn";
        cache.detected_at_ms = now_ms();
        write_web_search_region_cache(cache);

        if (notify) {
            notify("\xE2\x9A\xA0 Switched to " + fallback_name +
                   " (" + primary + " unreachable: " + err.message + ")");
        }
        return second;
    }
    // 双 fail:返回第二次的错误(更近的事实),不 notify
    return second;
}

nlohmann::json BackendRouter::status_snapshot(Region region) const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json j;
    j["active_backend"] = active_;
    j["config_backend"] = cfg_.backend;
    j["region"] = region_str(region);
    j["enabled"] = cfg_.enabled;
    nlohmann::json registered = nlohmann::json::array();
    for (const auto& [name, _] : backends_) registered.push_back(name);
    j["registered"] = std::move(registered);
    return j;
}

std::vector<std::string> BackendRouter::registered_names_for_test() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(backends_.size());
    for (const auto& [name, _] : backends_) out.push_back(name);
    return out;
}

void register_default_backends(BackendRouter& router, const WebSearchConfig& cfg) {
    router.register_backend(
        std::make_unique<DuckDuckGoBackend>(cfg.timeout_ms));
    router.register_backend(
        std::make_unique<BingCnBackend>(cfg.timeout_ms));

    if (cfg.backend == "bochaai" || cfg.backend == "tavily") {
        LOG_WARN("[web_search] backend '" + cfg.backend +
                 "' not implemented yet; falling back to auto behavior");
    }
    if (!is_known_backend_name(cfg.backend) && cfg.backend != "auto") {
        // load_config 应该已挡住,这里只是双保险。
        LOG_WARN("[web_search] unknown backend '" + cfg.backend +
                 "' in config; falling back to auto");
    }
}

} // namespace acecode::web_search
