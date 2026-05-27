#include "models_handler.hpp"

#include "../../config/model_provider_registry.hpp"

#include <algorithm>
#include <set>

namespace acecode::web {

namespace {

// 构造单个条目的 JSON。base_url / models_dev_provider_id 可选,有值才输出。
nlohmann::json entry_to_json(const ModelProfile& entry) {
    nlohmann::json o;
    o["name"]      = entry.name;
    o["provider"]  = entry.provider;
    o["model"]     = entry.model;
    if (!entry.base_url.empty()) o["base_url"] = entry.base_url;
    if (entry.models_dev_provider_id.has_value()) {
        o["models_dev_provider_id"] = *entry.models_dev_provider_id;
    }
    if (entry.context_window.has_value() && *entry.context_window > 0) {
        o["context_window"] = *entry.context_window;
    }
    if (entry.stream_timeout_ms.has_value() && *entry.stream_timeout_ms > 0) {
        o["stream_timeout_ms"] = *entry.stream_timeout_ms;
    }
    return o;
}

} // namespace

nlohmann::json list_models(const AppConfig& cfg) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& entry : cfg.saved_models) {
        if (!is_runtime_model_provider_enabled(entry.provider)) continue;
        arr.push_back(entry_to_json(entry));
    }
    return arr;
}

std::optional<ModelProfile>
find_model_by_name(const AppConfig& cfg, const std::string& name) {
    for (const auto& entry : cfg.saved_models) {
        if (entry.name == name && is_runtime_model_provider_enabled(entry.provider)) return entry;
    }
    return std::nullopt;
}

nlohmann::json model_state_to_json(const SessionModelState& state) {
    nlohmann::json o;
    o["name"] = state.name;
    o["provider"] = state.provider;
    o["model"] = state.model;
    o["context_window"] = state.context_window;
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
    if (!entry.base_url.empty()) o["base_url"] = entry.base_url;
    if (entry.models_dev_provider_id.has_value()) {
        o["models_dev_provider_id"] = *entry.models_dev_provider_id;
    }
    if (entry.context_window.has_value() && *entry.context_window > 0) {
        o["context_window"] = *entry.context_window;
    }
    if (entry.stream_timeout_ms.has_value() && *entry.stream_timeout_ms > 0) {
        o["stream_timeout_ms"] = *entry.stream_timeout_ms;
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
    if (body.contains("context_window") && body["context_window"].is_number_integer()) {
        d.context_window = body["context_window"].get<int>();
    }
    if (body.contains("stream_timeout_ms") && body["stream_timeout_ms"].is_number_integer()) {
        d.stream_timeout_ms = body["stream_timeout_ms"].get<int>();
    }
    if (body.contains("models_dev_provider_id") &&
        body["models_dev_provider_id"].is_string()) {
        std::string s = body["models_dev_provider_id"].get<std::string>();
        if (!s.empty()) d.models_dev_provider_id = std::move(s);
    }
    if (!err.empty()) return std::nullopt;
    return d;
}

std::optional<ModelProbeRequest> parse_model_probe_request(const nlohmann::json& body,
                                                           std::string& err_code,
                                                           std::string& err) {
    if (!body.is_object()) {
        err_code = "BAD_REQUEST";
        err = "body must be a JSON object";
        return std::nullopt;
    }

    auto string_value = [&](const char* key) -> std::string {
        if (!body.contains(key) || !body[key].is_string()) return {};
        return body[key].get<std::string>();
    };

    ModelProbeRequest request;
    request.provider = string_value("provider");
    request.base_url = string_value("base_url");
    request.api_key = string_value("api_key");

    if (request.provider.empty()) request.provider = "openai";
    if (request.provider != "openai" && request.provider != "copilot") {
        err_code = "UNKNOWN_PROVIDER";
        err = "model probing currently supports provider=openai or provider=copilot";
        return std::nullopt;
    }
    if (request.provider == "openai" && request.base_url.empty()) {
        err_code = "MISSING_BASE_URL";
        err = "base_url is required";
        return std::nullopt;
    }
    return request;
}

std::vector<std::string> parse_openai_model_ids(const nlohmann::json& body) {
    const nlohmann::json* list = nullptr;
    if (body.is_object()) {
        if (body.contains("data") && body["data"].is_array()) {
            list = &body["data"];
        } else if (body.contains("models") && body["models"].is_array()) {
            list = &body["models"];
        }
    } else if (body.is_array()) {
        list = &body;
    }
    if (!list) return {};

    std::set<std::string> unique;
    for (const auto& item : *list) {
        if (item.is_string()) {
            auto value = item.get<std::string>();
            if (!value.empty()) unique.insert(std::move(value));
            continue;
        }
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
            continue;
        }
        auto value = item["id"].get<std::string>();
        if (!value.empty()) unique.insert(std::move(value));
    }
    return {unique.begin(), unique.end()};
}

} // namespace acecode::web
