#include "model_pool_status.hpp"

#include "../network/proxy_resolver.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <utility>

#include <cpr/cpr.h>

namespace acecode {

std::unordered_map<std::string, ModelPoolStatus>
parse_model_pool_status(const std::string& body) {
    std::unordered_map<std::string, ModelPoolStatus> out;
    if (body.empty()) return out;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (...) {
        return out;  // 非法 JSON:安全返回空
    }
    if (!j.is_object()) return out;

    auto data = j.find("data");
    if (data == j.end() || !data->is_array()) return out;

    for (const auto& entry : *data) {
        if (!entry.is_object()) continue;
        auto name_it = entry.find("modelPoolName");
        if (name_it == entry.end() || !name_it->is_string()) continue;

        ModelPoolStatus status;
        if (auto u = entry.find("usageRate"); u != entry.end() && u->is_number()) {
            status.usage_rate = static_cast<int>(std::llround(u->get<double>()));
        }
        // 顶层 maxWindowTokens(池窗口,如 150000),不是 generateParam 里的嵌套值。
        if (auto m = entry.find("maxWindowTokens"); m != entry.end() && m->is_number()) {
            status.max_window_tokens = static_cast<long long>(m->get<double>());
        }
        out[name_it->get<std::string>()] = status;
    }
    return out;
}

ModelLoadTier model_load_tier(int usage_rate) {
    if (usage_rate < 0) return ModelLoadTier::Unknown;
    if (usage_rate > 90) return ModelLoadTier::Red;
    if (usage_rate >= 70) return ModelLoadTier::Yellow;  // 70..90 黄
    return ModelLoadTier::Green;                          // <70 绿
}

int effective_context_window(long long max_window_tokens) {
    if (max_window_tokens <= 0) return 0;
    return static_cast<int>(std::llround(0.8 * static_cast<double>(max_window_tokens)));
}

bool is_pub_model(const std::string& model) {
    if (model.size() < 3) return false;
    auto up = [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    };
    return up(model[0]) == 'P' && up(model[1]) == 'U' && up(model[2]) == 'B';
}

ModelPoolFetchResult default_model_pool_fetch(const std::string& url) {
    ModelPoolFetchResult r;
    auto opts = network::proxy_options_for(url);
    cpr::Response resp = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"Accept", "application/json"}},
        network::build_ssl_options(opts),
        opts.proxies,
        opts.auth,
        cpr::Timeout{8000});
    r.status_code = resp.status_code;
    r.body = resp.text;
    if (resp.status_code == 0) {
        r.error = resp.error.message;
    }
    return r;
}

ModelPoolStatusService::ModelPoolStatusService(ModelPoolFetchFn fetch, std::string url)
    : fetch_(fetch ? std::move(fetch) : ModelPoolFetchFn(&default_model_pool_fetch)),
      url_(url.empty() ? std::string(kModelPoolStatusUrl) : std::move(url)) {}

ModelPoolStatusService::~ModelPoolStatusService() { stop(); }

bool ModelPoolStatusService::refresh_once() {
    ModelPoolFetchResult fetched = fetch_(url_);
    if (fetched.status_code != 0 && (fetched.status_code < 200 || fetched.status_code >= 300)) {
        LOG_DEBUG("[model_pool] fetch http status=" + std::to_string(fetched.status_code));
        return false;
    }
    if (fetched.status_code == 0 && !fetched.error.empty()) {
        LOG_DEBUG("[model_pool] fetch transport error: " + fetched.error);
        return false;
    }
    auto parsed = parse_model_pool_status(fetched.body);
    if (parsed.empty()) {
        LOG_DEBUG("[model_pool] parsed 0 pools (empty / unexpected JSON)");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        cache_ = std::move(parsed);
    }
    return true;
}

void ModelPoolStatusService::start(std::function<void()> on_update) {
    if (running_.exchange(true)) return;  // 已在跑
    thread_ = std::thread([this, on_update = std::move(on_update)]() mutable {
        run_loop(std::move(on_update));
    });
}

void ModelPoolStatusService::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void ModelPoolStatusService::run_loop(std::function<void()> on_update) {
    while (running_.load()) {
        const bool ok = refresh_once();
        if (ok && on_update) on_update();
        std::unique_lock<std::mutex> lk(cv_mu_);
        cv_.wait_for(lk, kPollInterval, [this]() { return !running_.load(); });
    }
}

std::unordered_map<std::string, ModelPoolStatus>
ModelPoolStatusService::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_;
}

std::optional<ModelPoolStatus>
ModelPoolStatusService::get(const std::string& model_pool_name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(model_pool_name);
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}

int ModelPoolStatusService::effective_context_window_for(const std::string& model) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(model);
    if (it == cache_.end()) return 0;
    return effective_context_window(it->second.max_window_tokens);
}

ModelPoolStatusService& model_pool_status_service() {
    static ModelPoolStatusService instance;
    return instance;
}

} // namespace acecode
