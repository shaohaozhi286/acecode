#include "models_handler.hpp"

#include "../../provider/model_resolver.hpp"

namespace acecode::web {

namespace {

// 构造单个条目的 JSON。base_url / models_dev_provider_id 可选,有值才输出。
nlohmann::json entry_to_json(const ModelProfile& entry, bool is_legacy) {
    nlohmann::json o;
    o["name"]      = entry.name;
    o["provider"]  = entry.provider;
    o["model"]     = entry.model;
    o["is_legacy"] = is_legacy;
    if (!entry.base_url.empty()) o["base_url"] = entry.base_url;
    if (entry.models_dev_provider_id.has_value()) {
        o["models_dev_provider_id"] = *entry.models_dev_provider_id;
    }
    return o;
}

} // namespace

nlohmann::json list_models(const AppConfig& cfg) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& entry : cfg.saved_models) {
        arr.push_back(entry_to_json(entry, /*is_legacy=*/false));
    }
    arr.push_back(entry_to_json(synth_legacy_entry(cfg), /*is_legacy=*/true));
    return arr;
}

std::optional<ModelProfile>
find_model_by_name(const AppConfig& cfg, const std::string& name) {
    if (name == "(legacy)") {
        return synth_legacy_entry(cfg);
    }
    for (const auto& entry : cfg.saved_models) {
        if (entry.name == name) return entry;
    }
    return std::nullopt;
}

} // namespace acecode::web
