// saved_models 实现:纯函数 parse + validate。
// 对应 openspec/changes/model-profiles 任务 1.2。
#include "config/saved_models.hpp"

#include "model_provider_registry.hpp"
#include "request_headers.hpp"

#include <cctype>
#include <set>
#include <sstream>

namespace acecode {

namespace {

bool is_valid_capability_tag(const std::string& tag) {
    if (tag.empty()) return false;
    for (unsigned char ch : tag) {
        if (std::iscntrl(ch)) return false;
    }
    return true;
}

std::vector<std::string> parse_capabilities_array(const nlohmann::json& node) {
    std::vector<std::string> out;
    if (!node.is_array()) return out;
    for (const auto& item : node) {
        if (!item.is_string()) continue;
        std::string tag = item.get<std::string>();
        if (tag.empty()) continue;
        out.push_back(std::move(tag));
    }
    return out;
}

// 单条 entry 解析。失败时把错误塞进 err(已带 entry 索引/name 上下文)并返回 nullopt。
std::optional<ModelProfile> parse_one_entry(const nlohmann::json& node, std::size_t idx,
                                          std::string& err) {
    if (!node.is_object()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] is not an object";
        err = oss.str();
        return std::nullopt;
    }

    ModelProfile e;
    auto get_str = [&](const char* key, std::string& out) -> bool {
        if (!node.contains(key)) return false;
        if (!node[key].is_string()) return false;
        out = node[key].get<std::string>();
        return true;
    };

    if (!get_str("name", e.name) || e.name.empty()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] missing required field 'name'";
        err = oss.str();
        return std::nullopt;
    }
    if (!get_str("provider", e.provider) || e.provider.empty()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] (name='" << e.name
            << "') missing required field 'provider'";
        err = oss.str();
        return std::nullopt;
    }
    if (!is_known_model_provider(e.provider)) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] (name='" << e.name
            << "') has unknown provider '" << e.provider
            << "' (expected 'openai', 'anthropic', 'copilot', or 'codex')";
        err = oss.str();
        return std::nullopt;
    }
    if (!get_str("model", e.model) || e.model.empty()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] (name='" << e.name
            << "') missing required field 'model'";
        err = oss.str();
        return std::nullopt;
    }

    // base_url / api_key:openai/anthropic 必填;copilot/codex 忽略(允许字段缺失或为空)。
    get_str("base_url", e.base_url);
    get_str("api_key", e.api_key);

    if (node.contains("models_dev_provider_id") &&
        node["models_dev_provider_id"].is_string()) {
        std::string s = node["models_dev_provider_id"].get<std::string>();
        if (!s.empty()) e.models_dev_provider_id = std::move(s);
    }
    if (node.contains("context_window") && node["context_window"].is_number_integer()) {
        int context_window = node["context_window"].get<int>();
        if (context_window <= 0) {
            std::ostringstream oss;
            oss << "saved_models[" << idx << "] (name='" << e.name
                << "') has invalid context_window";
            err = oss.str();
            return std::nullopt;
        }
        e.context_window = context_window;
    }
    if (node.contains("stream_timeout_ms") && node["stream_timeout_ms"].is_number_integer()) {
        int stream_timeout_ms = node["stream_timeout_ms"].get<int>();
        if (stream_timeout_ms <= 0) {
            std::ostringstream oss;
            oss << "saved_models[" << idx << "] (name='" << e.name
                << "') has invalid stream_timeout_ms";
            err = oss.str();
            return std::nullopt;
        }
        e.stream_timeout_ms = stream_timeout_ms;
    }
    if (node.contains("capabilities")) {
        e.capabilities = parse_capabilities_array(node["capabilities"]);
    }
    if (node.contains("request_headers")) {
        std::ostringstream context;
        context << "saved_models[" << idx << "] (name='" << e.name << "')";
        auto parsed = parse_request_headers_json(node["request_headers"], context.str(), err);
        if (!parsed.has_value()) return std::nullopt;
        e.request_headers = std::move(*parsed);
    }
    if (node.contains("readonly") && node["readonly"].is_boolean()) {
        e.readonly = node["readonly"].get<bool>();
    }

    return e;
}

} // namespace

std::optional<std::vector<ModelProfile>> parse_saved_models(const nlohmann::json& node,
                                                          std::string& err) {
    std::vector<ModelProfile> out;
    if (node.is_null()) return out;
    if (!node.is_array()) {
        err = "saved_models must be a JSON array";
        return std::nullopt;
    }
    out.reserve(node.size());
    for (std::size_t i = 0; i < node.size(); ++i) {
        auto entry = parse_one_entry(node[i], i, err);
        if (!entry.has_value()) return std::nullopt;
        out.push_back(std::move(*entry));
    }
    return out;
}

bool validate_saved_models(const std::vector<ModelProfile>& entries,
                           const std::string& default_name,
                           std::string& err) {
    std::set<std::string> seen_names;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.name.empty()) {
            std::ostringstream oss;
            oss << "saved_models[" << i << "] has empty name";
            err = oss.str();
            return false;
        }
        if (!e.name.empty() && e.name.front() == '(') {
            std::ostringstream oss;
            oss << "saved_models[" << i << "] name '" << e.name
                << "' uses reserved prefix '(' (reserved for ACECode-synthesized "
                   "names like \"(session:...)\")";
            err = oss.str();
            return false;
        }
        if (seen_names.count(e.name)) {
            std::ostringstream oss;
            oss << "saved_models has duplicate name '" << e.name << "'";
            err = oss.str();
            return false;
        }
        seen_names.insert(e.name);

        if (e.provider == "openai" || e.provider == "anthropic") {
            if (e.base_url.empty()) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' (provider=" << e.provider << ") requires non-empty base_url";
                err = oss.str();
                return false;
            }
            if (e.api_key.empty()) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' (provider=" << e.provider << ") requires non-empty api_key";
                err = oss.str();
                return false;
            }
        } else if (!e.request_headers.empty()) {
            std::ostringstream oss;
            oss << "saved_models entry '" << e.name
                << "' has request_headers but provider is not openai or anthropic";
            err = oss.str();
            return false;
        }
        if (e.context_window.has_value() && *e.context_window <= 0) {
            std::ostringstream oss;
            oss << "saved_models entry '" << e.name
                << "' has invalid context_window";
            err = oss.str();
            return false;
        }
        if (e.stream_timeout_ms.has_value() && *e.stream_timeout_ms <= 0) {
            std::ostringstream oss;
            oss << "saved_models entry '" << e.name
                << "' has invalid stream_timeout_ms";
            err = oss.str();
            return false;
        }
        std::set<std::string> seen_capabilities;
        for (const auto& tag : e.capabilities) {
            if (!is_valid_capability_tag(tag)) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' has invalid capability tag";
                err = oss.str();
                return false;
            }
            if (!seen_capabilities.insert(tag).second) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' has duplicate capability '" << tag << "'";
                err = oss.str();
                return false;
            }
        }
        if (!e.request_headers.empty()) {
            std::string headers_err;
            if (!validate_request_headers(e.request_headers, headers_err)) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name << "' invalid "
                    << headers_err;
                err = oss.str();
                return false;
            }
        }
        // copilot/codex:不需要 base_url / api_key,model 已在 parse 阶段检验非空。
    }

    if (!default_name.empty()) {
        if (!seen_names.count(default_name)) {
            std::ostringstream oss;
            oss << "default_model_name '" << default_name
                << "' does not match any saved_models entry";
            err = oss.str();
            return false;
        }
    }
    return true;
}

} // namespace acecode
