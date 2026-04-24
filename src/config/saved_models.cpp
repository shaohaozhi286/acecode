// saved_models 实现:纯函数 parse + validate。
// 对应 openspec/changes/model-profiles 任务 1.2。
#include "config/saved_models.hpp"

#include <set>
#include <sstream>

namespace acecode {

namespace {

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
    if (e.provider != "openai" && e.provider != "copilot") {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] (name='" << e.name
            << "') has unknown provider '" << e.provider
            << "' (expected 'openai' or 'copilot')";
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

    // base_url / api_key:openai 必填;copilot 忽略(允许字段缺失或为空)。
    get_str("base_url", e.base_url);
    get_str("api_key", e.api_key);

    if (node.contains("models_dev_provider_id") &&
        node["models_dev_provider_id"].is_string()) {
        std::string s = node["models_dev_provider_id"].get<std::string>();
        if (!s.empty()) e.models_dev_provider_id = std::move(s);
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
                   "names like \"(legacy)\" / \"(session:...)\")";
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

        if (e.provider == "openai") {
            if (e.base_url.empty()) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' (provider=openai) requires non-empty base_url";
                err = oss.str();
                return false;
            }
            if (e.api_key.empty()) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' (provider=openai) requires non-empty api_key";
                err = oss.str();
                return false;
            }
        }
        // copilot:不需要 base_url / api_key,model 已在 parse 阶段检验非空。
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
