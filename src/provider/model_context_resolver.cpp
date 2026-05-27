#include "model_context_resolver.hpp"

#include "../network/proxy_resolver.hpp"
#include "../utils/logger.hpp"
#include "../codex/codex_model_catalog.hpp"
#include "models_dev_registry.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acecode {
namespace {

std::mutex g_context_cache_mu;
std::map<std::string, int> g_context_cache;
std::set<std::string> g_context_probe_in_flight;

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string strip_provider_prefix(const std::string& model) {
    auto pos = model.find('/');
    if (pos == std::string::npos || pos + 1 >= model.size()) {
        return model;
    }
    return model.substr(pos + 1);
}

bool model_matches(const std::string& candidate, const std::string& target) {
    if (candidate.empty() || target.empty()) {
        return false;
    }

    const std::string normalized_candidate = to_lower_copy(candidate);
    const std::string normalized_target = to_lower_copy(target);
    if (normalized_candidate == normalized_target) {
        return true;
    }

    return strip_provider_prefix(normalized_candidate) == strip_provider_prefix(normalized_target);
}

int json_int_value(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<int>(value.get<unsigned int>());
    }
    if (value.is_number_float()) {
        return static_cast<int>(value.get<double>());
    }
    return 0;
}

int extract_context_length(const nlohmann::json& value) {
    static const std::vector<std::string> keys = {
        "context_length",
        "context_window",
        "max_context_length",
        "max_input_tokens",
        "input_token_limit",
        "input_tokens",
        "context",
        "input"
    };

    if (value.is_object()) {
        for (const auto& key : keys) {
            auto it = value.find(key);
            if (it != value.end()) {
                int parsed = json_int_value(*it);
                if (parsed > 0) {
                    return parsed;
                }
            }
        }

        for (const auto& item : value.items()) {
            int parsed = extract_context_length(item.value());
            if (parsed > 0) {
                return parsed;
            }
        }
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            int parsed = extract_context_length(item);
            if (parsed > 0) {
                return parsed;
            }
        }
    }

    return 0;
}

const nlohmann::json* find_model_entry(const nlohmann::json& models, const std::string& model) {
    if (models.is_array()) {
        for (const auto& item : models) {
            if (!item.is_object()) {
                continue;
            }
            if (model_matches(item.value("id", ""), model)) {
                return &item;
            }
        }
    }

    if (models.is_object()) {
        for (const auto& item : models.items()) {
            if (model_matches(item.key(), model)) {
                return &item.value();
            }
            if (item.value().is_object() && model_matches(item.value().value("id", ""), model)) {
                return &item.value();
            }
        }
    }

    return nullptr;
}

int lookup_models_dev_context(const std::string& provider_id, const std::string& model) {
    if (provider_id.empty() || model.empty()) {
        return 0;
    }

    auto registry = current_registry();
    if (!registry) return 0;

    const nlohmann::json* provider = find_provider_entry(*registry, provider_id);
    if (!provider) return 0;

    auto models_it = provider->find("models");
    if (models_it == provider->end()) {
        return 0;
    }

    const nlohmann::json* entry = find_model_entry(*models_it, model);
    if (!entry) {
        return 0;
    }

    int context = extract_context_length(*entry);
    if (context > 0) {
        LOG_INFO("Resolved model context via models.dev provider=" + provider_id +
                 " model=" + model + " context=" + std::to_string(context));
    }
    return context;
}

int fetch_models_endpoint_context(const std::string& base_url,
                                  const std::string& api_key,
                                  const std::string& model) {
    if (base_url.empty() || model.empty()) {
        return 0;
    }

    const std::string url = trim_trailing_slash(base_url) + "/models";
    cpr::Header headers = {{"Content-Type", "application/json"}};
    if (!api_key.empty()) {
        headers["Authorization"] = "Bearer " + api_key;
    }

    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response response = cpr::Get(
        cpr::Url{url},
        headers,
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{15000}
    );

    if (response.status_code != 200) {
        return 0;
    }

    try {
        nlohmann::json parsed = nlohmann::json::parse(response.text);
        const nlohmann::json* entry = nullptr;

        if (parsed.is_object() && parsed.contains("data")) {
            entry = find_model_entry(parsed["data"], model);
        }
        if (!entry) {
            entry = find_model_entry(parsed, model);
        }
        if (!entry) {
            return 0;
        }

        int context = extract_context_length(*entry);
        if (context > 0) {
            LOG_INFO("Resolved model context via endpoint model=" + model +
                     " context=" + std::to_string(context));
        }
        return context;
    } catch (const std::exception& ex) {
        LOG_WARN(std::string("Failed to parse /models metadata: ") + ex.what());
        return 0;
    }
}

std::string detect_models_dev_provider(const AppConfig& config, const std::string& provider_name) {
    // Explicit hint from configure wizard / catalog selection wins over heuristics.
    if (config.openai.models_dev_provider_id.has_value() &&
        !config.openai.models_dev_provider_id->empty()) {
        return *config.openai.models_dev_provider_id;
    }

    const std::string normalized_provider = to_lower_copy(provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "copilot") {
        return "github-copilot";
    }

    if (normalized_provider == "openai") {
        const std::string base_url = to_lower_copy(config.openai.base_url);
        if (base_url.find("api.openai.com") != std::string::npos) {
            return "openai";
        }
    }

    // The provider_name argument is also accepted as a direct models.dev id
    // (e.g. "anthropic", "openrouter") so callers can short-circuit detection.
    if (!normalized_provider.empty() && normalized_provider != "openai" &&
        normalized_provider != "copilot") {
        return normalized_provider;
    }

    return "";
}

std::string context_cache_key(const AppConfig& config,
                              const std::string& provider_name,
                              const std::string& model) {
    const std::string normalized_provider = to_lower_copy(
        provider_name.empty() ? config.provider : provider_name);
    std::string base_url;
    std::string provider_hint;
    if (normalized_provider == "openai") {
        base_url = to_lower_copy(trim_trailing_slash(config.openai.base_url));
        if (config.openai.models_dev_provider_id.has_value()) {
            provider_hint = to_lower_copy(*config.openai.models_dev_provider_id);
        }
    }
    return normalized_provider + "\n" + to_lower_copy(model) + "\n" +
           base_url + "\n" + provider_hint;
}

AppConfig config_for_profile_context(const AppConfig& cfg,
                                     const ModelProfile& profile) {
    AppConfig context_cfg = cfg;
    context_cfg.provider = profile.provider;
    if (profile.provider == "openai") {
        context_cfg.openai.base_url = profile.base_url;
        context_cfg.openai.api_key = profile.api_key;
        context_cfg.openai.model = profile.model;
        context_cfg.openai.models_dev_provider_id = profile.models_dev_provider_id;
    } else if (profile.provider == "codex") {
        context_cfg.codex.model = profile.model;
    } else {
        context_cfg.copilot.model = profile.model;
    }
    return context_cfg;
}

int cached_context(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    auto it = g_context_cache.find(key);
    return it == g_context_cache.end() ? 0 : it->second;
}

void remember_context(const std::string& key, int context) {
    if (context <= 0) return;
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    g_context_cache[key] = context;
}

int cached_or_local_context(const AppConfig& config,
                            const std::string& provider_name,
                            const std::string& model) {
    const std::string key = context_cache_key(config, provider_name, model);
    if (int context = cached_context(key); context > 0) {
        return context;
    }

    const std::string normalized_provider = to_lower_copy(
        provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "codex") {
        int context = codex::context_window_for_model(model);
        if (context > 0) {
            remember_context(key, context);
            return context;
        }
    }

    const std::string models_dev_provider = detect_models_dev_provider(config, provider_name);
    if (!models_dev_provider.empty()) {
        int context = lookup_models_dev_context(models_dev_provider, model);
        if (context > 0) {
            remember_context(key, context);
            return context;
        }
    }

    return 0;
}

bool mark_probe_in_flight(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    if (g_context_cache.find(key) != g_context_cache.end()) return false;
    return g_context_probe_in_flight.insert(key).second;
}

void clear_probe_in_flight(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    g_context_probe_in_flight.erase(key);
}

void warm_context_async(AppConfig config,
                        std::string provider_name,
                        std::string model) {
    const std::string normalized_provider = to_lower_copy(
        provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider != "openai" || config.openai.base_url.empty() || model.empty()) {
        return;
    }

    const std::string key = context_cache_key(config, provider_name, model);
    if (!mark_probe_in_flight(key)) return;

    std::thread([config = std::move(config),
                 provider_name = std::move(provider_name),
                 model = std::move(model),
                 key]() mutable {
        try {
            int context = fetch_models_endpoint_context(config.openai.base_url,
                                                       config.openai.api_key,
                                                       model);
            if (context > 0) {
                remember_context(key, context);
            }
        } catch (const std::exception& ex) {
            LOG_WARN(std::string("Background model context probe failed: ") + ex.what());
        } catch (...) {
            LOG_WARN("Background model context probe failed with unknown error");
        }
        clear_probe_in_flight(key);
    }).detach();
}

} // namespace

int resolve_model_context_window(const AppConfig& config,
                                 const std::string& provider_name,
                                 const std::string& model,
                                 int fallback_context_window) {
    const std::string key = context_cache_key(config, provider_name, model);
    if (int context = cached_or_local_context(config, provider_name, model); context > 0) {
        return context;
    }

    const std::string normalized_provider = to_lower_copy(provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "openai") {
        int context = fetch_models_endpoint_context(config.openai.base_url, config.openai.api_key, model);
        if (context > 0) {
            remember_context(key, context);
            return context;
        }
    }

    return fallback_context_window;
}

int resolve_model_context_window_nonblocking(const AppConfig& config,
                                             const std::string& provider_name,
                                             const std::string& model,
                                             int fallback_context_window) {
    if (int context = cached_or_local_context(config, provider_name, model); context > 0) {
        return context;
    }
    warm_context_async(config, provider_name, model);
    return fallback_context_window;
}

int resolve_model_profile_context_window(const AppConfig& config,
                                         const ModelProfile& profile,
                                         int fallback_context_window) {
    if (profile.context_window.has_value() && *profile.context_window > 0) {
        return *profile.context_window;
    }
    auto context_cfg = config_for_profile_context(config, profile);
    return resolve_model_context_window(
        context_cfg, profile.provider, profile.model, fallback_context_window);
}

int resolve_model_profile_context_window_nonblocking(const AppConfig& config,
                                                     const ModelProfile& profile,
                                                     int fallback_context_window) {
    if (profile.context_window.has_value() && *profile.context_window > 0) {
        return *profile.context_window;
    }
    auto context_cfg = config_for_profile_context(config, profile);
    return resolve_model_context_window_nonblocking(
        context_cfg, profile.provider, profile.model, fallback_context_window);
}

void reset_model_context_window_cache_for_test() {
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    g_context_cache.clear();
    g_context_probe_in_flight.clear();
}

} // namespace acecode
