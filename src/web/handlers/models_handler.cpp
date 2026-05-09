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

nlohmann::json model_state_to_json(const SessionModelState& state) {
    nlohmann::json o;
    o["name"] = state.name;
    o["provider"] = state.provider;
    o["model"] = state.model;
    o["context_window"] = state.context_window;
    o["is_legacy"] = state.is_legacy;
    return o;
}

int http_status_for_edit_error(SavedModelEditError e) {
    switch (e) {
        case SavedModelEditError::OK: return 200;
        case SavedModelEditError::NOT_FOUND: return 404;
        case SavedModelEditError::NAME_TAKEN:
        case SavedModelEditError::IN_USE_AS_DEFAULT: return 409;
        default: return 400;
    }
}

nlohmann::json profile_to_safe_json(const ModelProfile& entry) {
    nlohmann::json o;
    o["name"]      = entry.name;
    o["provider"]  = entry.provider;
    o["model"]     = entry.model;
    o["is_legacy"] = entry.name == "(legacy)";
    if (!entry.base_url.empty()) o["base_url"] = entry.base_url;
    if (entry.models_dev_provider_id.has_value()) {
        o["models_dev_provider_id"] = *entry.models_dev_provider_id;
    }
    // api_key 永不输出 — 安全契约
    return o;
}

std::optional<SavedModelDraft> parse_model_draft(const nlohmann::json& body,
                                                  std::string& err) {
    if (!body.is_object()) { err = "body must be a JSON object"; return std::nullopt; }
    SavedModelDraft d;
    // 注意:可选字段碰到 null / 非 string 静默跳过(out 保持默认空串),
    // 与下方 models_dev_provider_id 的处理保持一致;只有必填字段才会硬性
    // 报错。前端 form 经常把空 input 序列化成 null,这里宽容处理避免误拒。
    auto get_str = [&](const char* key, std::string& out, bool required) {
        if (!body.contains(key)) {
            if (required && err.empty()) {
                err = std::string("missing field '") + key + "'";
            }
            return;
        }
        if (!body[key].is_string()) {
            if (required && err.empty()) {
                err = std::string("field '") + key + "' must be string";
            }
            return;
        }
        out = body[key].get<std::string>();
    };
    get_str("name", d.name, true);
    get_str("provider", d.provider, true);
    get_str("model", d.model, true);
    get_str("base_url", d.base_url, false);
    get_str("api_key", d.api_key, false);
    if (body.contains("models_dev_provider_id") &&
        body["models_dev_provider_id"].is_string()) {
        std::string s = body["models_dev_provider_id"].get<std::string>();
        if (!s.empty()) d.models_dev_provider_id = std::move(s);
    }
    if (!err.empty()) return std::nullopt;
    return d;
}

} // namespace acecode::web
