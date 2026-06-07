// src/config/saved_models_editor.cpp
#include "saved_models_editor.hpp"

#include "model_provider_registry.hpp"
#include "request_headers.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace acecode {

const char* to_string(SavedModelEditError e) {
    switch (e) {
        case SavedModelEditError::OK:                return "OK";
        case SavedModelEditError::INVALID_NAME:      return "INVALID_NAME";
        case SavedModelEditError::RESERVED_NAME:     return "RESERVED_NAME";
        case SavedModelEditError::NAME_TAKEN:        return "NAME_TAKEN";
        case SavedModelEditError::UNKNOWN_PROVIDER:  return "UNKNOWN_PROVIDER";
        case SavedModelEditError::PROVIDER_DISABLED: return "PROVIDER_DISABLED";
        case SavedModelEditError::MISSING_MODEL:     return "MISSING_MODEL";
        case SavedModelEditError::MISSING_BASE_URL:  return "MISSING_BASE_URL";
        case SavedModelEditError::INVALID_API_KEY:   return "INVALID_API_KEY";
        case SavedModelEditError::INVALID_CONTEXT_WINDOW: return "INVALID_CONTEXT_WINDOW";
        case SavedModelEditError::INVALID_STREAM_TIMEOUT: return "INVALID_STREAM_TIMEOUT";
        case SavedModelEditError::INVALID_CAPABILITY: return "INVALID_CAPABILITY";
        case SavedModelEditError::INVALID_REQUEST_HEADER: return "INVALID_REQUEST_HEADER";
        case SavedModelEditError::NOT_FOUND:         return "NOT_FOUND";
        case SavedModelEditError::IN_USE_AS_DEFAULT: return "IN_USE_AS_DEFAULT";
    }
    return "UNKNOWN";
}

namespace {

bool valid_capability_tag(const std::string& tag) {
    if (tag.empty()) return false;
    for (unsigned char ch : tag) {
        if (std::iscntrl(ch)) return false;
    }
    return true;
}

SavedModelEditError validate_draft_basic(const SavedModelDraft& d) {
    if (d.name.empty()) return SavedModelEditError::INVALID_NAME;
    if (d.name.front() == '(') return SavedModelEditError::RESERVED_NAME;
    if (!is_known_model_provider(d.provider))
        return SavedModelEditError::UNKNOWN_PROVIDER;
    if (!is_runtime_model_provider_enabled(d.provider))
        return SavedModelEditError::PROVIDER_DISABLED;
    if (d.model.empty()) return SavedModelEditError::MISSING_MODEL;
    if (d.provider == "openai") {
        if (d.base_url.empty()) return SavedModelEditError::MISSING_BASE_URL;
        if (d.api_key.empty()) return SavedModelEditError::INVALID_API_KEY;
    }
    if (d.context_window.has_value() && *d.context_window < 0) {
        return SavedModelEditError::INVALID_CONTEXT_WINDOW;
    }
    if (d.stream_timeout_ms.has_value() && *d.stream_timeout_ms < 0) {
        return SavedModelEditError::INVALID_STREAM_TIMEOUT;
    }
    std::set<std::string> seen_capabilities;
    for (const auto& tag : d.capabilities) {
        if (!valid_capability_tag(tag)) return SavedModelEditError::INVALID_CAPABILITY;
        if (!seen_capabilities.insert(tag).second) return SavedModelEditError::INVALID_CAPABILITY;
    }
    if (!d.request_headers.empty()) {
        if (d.provider != "openai") return SavedModelEditError::INVALID_REQUEST_HEADER;
        std::string err;
        if (!validate_request_headers(d.request_headers, err)) {
            return SavedModelEditError::INVALID_REQUEST_HEADER;
        }
    }
    return SavedModelEditError::OK;
}

ModelProfile to_profile(const SavedModelDraft& d) {
    ModelProfile p;
    p.name = d.name;
    p.provider = d.provider;
    p.model = d.model;
    if (d.provider == "openai") {
        p.base_url = d.base_url;
        p.api_key = d.api_key;
    }
    p.models_dev_provider_id = d.models_dev_provider_id;
    if (d.context_window.has_value() && *d.context_window > 0) {
        p.context_window = *d.context_window;
    }
    if (d.stream_timeout_ms.has_value() && *d.stream_timeout_ms > 0) {
        p.stream_timeout_ms = *d.stream_timeout_ms;
    }
    p.capabilities = d.capabilities;
    p.request_headers = d.request_headers;
    return p;
}

bool name_exists(const AppConfig& cfg, const std::string& name) {
    for (const auto& e : cfg.saved_models) if (e.name == name) return true;
    return false;
}

} // namespace

SavedModelEditError add_saved_model(AppConfig& cfg, const SavedModelDraft& d) {
    if (auto err = validate_draft_basic(d); err != SavedModelEditError::OK) return err;
    if (name_exists(cfg, d.name)) return SavedModelEditError::NAME_TAKEN;
    cfg.saved_models.push_back(to_profile(d));
    return SavedModelEditError::OK;
}

SavedModelEditError update_saved_model(AppConfig& cfg,
                                        const std::string& old_name,
                                        const SavedModelDraft& d) {
    auto it = std::find_if(cfg.saved_models.begin(), cfg.saved_models.end(),
                            [&](const ModelProfile& e) { return e.name == old_name; });
    if (it == cfg.saved_models.end()) return SavedModelEditError::NOT_FOUND;

    // 改名:必须不与 default 冲突,新名也要走完整校验且不能撞别的现有 name。
    const bool renaming = d.name != old_name;
    if (renaming && cfg.default_model_name == old_name) {
        return SavedModelEditError::IN_USE_AS_DEFAULT;
    }
    if (auto err = validate_draft_basic(d); err != SavedModelEditError::OK) return err;
    if (renaming) {
        if (name_exists(cfg, d.name)) return SavedModelEditError::NAME_TAKEN;
    }

    *it = to_profile(d);
    return SavedModelEditError::OK;
}

SavedModelEditError remove_saved_model(AppConfig& cfg, const std::string& name) {
    auto it = std::find_if(cfg.saved_models.begin(), cfg.saved_models.end(),
                            [&](const ModelProfile& e) { return e.name == name; });
    if (it == cfg.saved_models.end()) return SavedModelEditError::NOT_FOUND;
    const bool removing_default = cfg.default_model_name == name;
    if (removing_default && cfg.saved_models.size() > 1) {
        return SavedModelEditError::IN_USE_AS_DEFAULT;
    }
    cfg.saved_models.erase(it);
    if (removing_default) cfg.default_model_name.clear();
    return SavedModelEditError::OK;
}

} // namespace acecode
