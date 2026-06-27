#include "models_handler.hpp"

#include "../../config/model_provider_registry.hpp"
#include "../../config/request_headers.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <string>

namespace acecode::web {

namespace {

std::optional<int> positive_int_from_json(const nlohmann::json& value) {
    long long parsed = 0;
    if (value.is_number_integer() || value.is_number_unsigned()) {
        parsed = value.get<long long>();
    } else if (value.is_number_float()) {
        const double d = value.get<double>();
        if (!std::isfinite(d)) return std::nullopt;
        parsed = static_cast<long long>(std::llround(d));
        if (std::fabs(d - static_cast<double>(parsed)) > 0.000001) return std::nullopt;
    } else if (value.is_string()) {
        const std::string s = value.get<std::string>();
        std::size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
        std::size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
        if (start == end) return std::nullopt;
        long long acc = 0;
        for (std::size_t i = start; i < end; ++i) {
            unsigned char ch = static_cast<unsigned char>(s[i]);
            if (!std::isdigit(ch)) return std::nullopt;
            acc = acc * 10 + static_cast<long long>(ch - '0');
            if (acc > std::numeric_limits<int>::max()) return std::nullopt;
        }
        parsed = acc;
    } else {
        return std::nullopt;
    }
    if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(parsed);
}

int context_window_from_model_item(const nlohmann::json& item) {
    if (!item.is_object()) return 0;
    static constexpr const char* kContextKeys[] = {
        "context_window",
        "contextWindow",
        "context_length",
        "contextLength",
        "max_context",
        "maxContext",
        "max_context_length",
        "maxContextLength",
        "max_context_tokens",
        "maxContextTokens",
        "max_input_tokens",
        "maxInputTokens",
        "input_token_limit",
        "inputTokenLimit",
        "max_window_tokens",
        "maxWindowTokens",
        "max_model_len",
        "maxModelLen",
        "n_ctx",
    };

    auto scan_object = [&](const nlohmann::json& obj) -> int {
        if (!obj.is_object()) return 0;
        for (const char* key : kContextKeys) {
            auto it = obj.find(key);
            if (it == obj.end()) continue;
            if (auto parsed = positive_int_from_json(*it)) return *parsed;
        }
        return 0;
    };

    if (int parsed = scan_object(item); parsed > 0) return parsed;
    for (const char* nested_key : {"metadata", "meta", "model_info", "modelInfo", "limits"}) {
        auto it = item.find(nested_key);
        if (it == item.end()) continue;
        if (int parsed = scan_object(*it); parsed > 0) return parsed;
    }
    return 0;
}

// 构造单个条目的 JSON。base_url / models_dev_provider_id 可选,有值才输出。
nlohmann::json entry_to_json(const ModelProfile& entry) {
    nlohmann::json o;
    o["name"]      = entry.name;
    o["provider"]  = entry.provider;
    o["model"]     = entry.model;
    if (!entry.base_url.empty()) o["base_url"] = entry.base_url;
    if (!entry.api_key.empty()) o["api_key"] = entry.api_key;
    if (entry.models_dev_provider_id.has_value()) {
        o["models_dev_provider_id"] = *entry.models_dev_provider_id;
    }
    if (entry.context_window.has_value() && *entry.context_window > 0) {
        o["context_window"] = *entry.context_window;
    }
    if (entry.stream_timeout_ms.has_value() && *entry.stream_timeout_ms > 0) {
        o["stream_timeout_ms"] = *entry.stream_timeout_ms;
    }
    if (!entry.capabilities.empty()) {
        o["capabilities"] = entry.capabilities;
    }
    if (!entry.request_headers.empty()) {
        o["request_headers"] = entry.request_headers;
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
        if (entry.name == name && is_runtime_model_provider_enabled(entry.provider)) {
            ModelProfile profile = entry;
            if (profile.provider == "openai" && !profile.stream_timeout_ms.has_value()) {
                profile.stream_timeout_ms = cfg.openai.stream_timeout_ms;
            }
            return profile;
        }
    }
    return std::nullopt;
}

nlohmann::json model_state_to_json(const SessionModelState& state) {
    nlohmann::json o;
    o["name"] = state.name;
    o["provider"] = state.provider;
    o["model"] = state.model;
    o["context_window"] = state.context_window;
    o["deleted"] = state.deleted;
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
    if (!entry.api_key.empty()) o["api_key"] = entry.api_key;
    if (entry.models_dev_provider_id.has_value()) {
        o["models_dev_provider_id"] = *entry.models_dev_provider_id;
    }
    if (entry.context_window.has_value() && *entry.context_window > 0) {
        o["context_window"] = *entry.context_window;
    }
    if (entry.stream_timeout_ms.has_value() && *entry.stream_timeout_ms > 0) {
        o["stream_timeout_ms"] = *entry.stream_timeout_ms;
    }
    if (!entry.capabilities.empty()) {
        o["capabilities"] = entry.capabilities;
    }
    if (!entry.request_headers.empty()) {
        o["request_headers"] = entry.request_headers;
    }
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
    if (body.contains("capabilities") && body["capabilities"].is_array()) {
        for (const auto& item : body["capabilities"]) {
            if (!item.is_string()) continue;
            std::string tag = item.get<std::string>();
            if (tag.empty()) continue;
            d.capabilities.push_back(std::move(tag));
        }
    }
    if (body.contains("request_headers")) {
        auto parsed = parse_request_headers_json(body["request_headers"],
                                                 "model draft",
                                                 err);
        if (!parsed.has_value()) return std::nullopt;
        d.request_headers = std::move(*parsed);
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
    if (body.contains("request_headers")) {
        auto parsed = parse_request_headers_json(body["request_headers"],
                                                 "model probe",
                                                 err);
        if (!parsed.has_value()) {
            err_code = "BAD_REQUEST";
            return std::nullopt;
        }
        request.request_headers = std::move(*parsed);
    }

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
    if (!request.request_headers.empty()) {
        if (request.provider != "openai") {
            err_code = "INVALID_REQUEST_HEADER";
            err = "request_headers are only supported for provider=openai";
            return std::nullopt;
        }
        if (!validate_request_headers(request.request_headers, err)) {
            err_code = "INVALID_REQUEST_HEADER";
            return std::nullopt;
        }
    }
    return request;
}

ParsedOpenAiModels parse_openai_models(const nlohmann::json& body) {
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
    std::map<std::string, int> context_windows;
    for (const auto& item : *list) {
        std::string value;
        if (item.is_string()) {
            value = item.get<std::string>();
        } else if (item.is_object()) {
            for (const char* key : {"id", "model", "name"}) {
                auto it = item.find(key);
                if (it != item.end() && it->is_string()) {
                    value = it->get<std::string>();
                    break;
                }
            }
        }
        if (value.empty()) continue;
        unique.insert(value);
        if (item.is_object()) {
            int context_window = context_window_from_model_item(item);
            if (context_window > 0) context_windows[value] = context_window;
        }
    }
    return {{unique.begin(), unique.end()}, std::move(context_windows)};
}

std::vector<std::string> parse_openai_model_ids(const nlohmann::json& body) {
    return parse_openai_models(body).ids;
}

} // namespace acecode::web
