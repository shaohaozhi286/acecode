// model_resolver 实现。见 design.md D2。
#include "model_resolver.hpp"

#include "../utils/logger.hpp"

namespace acecode {

namespace {

// 在 entries 中按 name 找,返回指针(nullptr = 未命中)。
const ModelProfile* find_by_name(const std::vector<ModelProfile>& entries,
                               const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& e : entries) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

// 在 entries 中按 (provider, model) 二元组匹配。spec 2.4 要求:session meta
// 匹配 saved_models 时用 (provider, model) 而不是 name,因为 meta 可能来自
// 本 change 之前的老 session(meta 里根本没 name 概念)。
const ModelProfile* find_by_provider_model(const std::vector<ModelProfile>& entries,
                                         const std::string& provider,
                                         const std::string& model) {
    for (const auto& e : entries) {
        if (e.provider == provider && e.model == model) return &e;
    }
    return nullptr;
}

// 构造 resume 时找不到匹配 entry 的 ad-hoc 兜底。best-effort 借 cfg.openai
// 的 base_url/api_key 给 openai 场景;copilot 不需要这俩。缺关键字段时走 log。
ModelProfile build_ad_hoc_entry(const AppConfig& cfg, const SessionMeta& meta) {
    ModelProfile e;
    std::string prefix = meta.id.size() >= 8 ? meta.id.substr(0, 8) : meta.id;
    e.name = "(session:" + prefix + ")";
    e.provider = meta.provider;
    e.model = meta.model;
    if (meta.provider == "openai") {
        e.base_url = cfg.openai.base_url;
        e.api_key = cfg.openai.api_key;
        if (e.base_url.empty()) {
            LOG_WARN("[model_resolver] ad-hoc entry '" + e.name +
                     "' has empty base_url (no usable fallback from cfg.openai); "
                     "consider /model --default <name> to pick a permanent one");
        }
    }
    return e;
}

} // namespace

ModelProfile synth_legacy_entry(const AppConfig& cfg) {
    ModelProfile e;
    e.name = "(legacy)";
    e.provider = cfg.provider;
    if (cfg.provider == "openai") {
        e.base_url = cfg.openai.base_url;
        e.api_key = cfg.openai.api_key;
        e.model = cfg.openai.model;
        e.models_dev_provider_id = cfg.openai.models_dev_provider_id;
    } else {
        // copilot(或未知 provider,按 copilot 走)
        e.model = cfg.copilot.model;
    }
    return e;
}

ModelProfile resolve_effective_model(const AppConfig& cfg,
                                   const std::optional<std::string>& cwd_override_name,
                                   const std::optional<SessionMeta>& resumed_meta) {
    // 第 1 层:global default
    std::string chosen_name = cfg.default_model_name;

    // 第 2 层:cwd override(非空字符串才算设了 override)
    if (cwd_override_name.has_value() && !cwd_override_name->empty()) {
        // 确认 override name 指向的 entry 存在;不存在时记 warning 并走第 1 层。
        if (find_by_name(cfg.saved_models, *cwd_override_name) != nullptr ||
            *cwd_override_name == "(legacy)") {
            chosen_name = *cwd_override_name;
        } else {
            LOG_WARN("[model_resolver] cwd override points to missing entry '" +
                     *cwd_override_name + "' (from model_override.json); "
                     "falling back to default/legacy");
        }
    }

    // 第 3 层:resume session meta —— 按 (provider, model) 查 saved_models。
    // 匹配不上时构造 ad-hoc entry,直接作为最终结果返回(绕开后续的按 name 查找)。
    if (resumed_meta.has_value() &&
        !resumed_meta->provider.empty() &&
        !resumed_meta->model.empty()) {
        const ModelProfile* matched = find_by_provider_model(
            cfg.saved_models, resumed_meta->provider, resumed_meta->model);
        if (matched != nullptr) {
            return *matched;
        }
        // 未匹配:看看 legacy entry 能不能兜住(老用户 session 记录的
        // provider/model 正好就是 legacy 字段本身)。
        ModelProfile legacy = synth_legacy_entry(cfg);
        if (legacy.provider == resumed_meta->provider &&
            legacy.model == resumed_meta->model) {
            return legacy;
        }
        // 仍未命中 —— ad-hoc 兜底(spec: name 以 "(session:" 开头,触发系统提示)。
        return build_ad_hoc_entry(cfg, *resumed_meta);
    }

    // 第 4 层:按 chosen_name 查 saved_models;命中返回,否则 legacy。
    if (chosen_name == "(legacy)") {
        return synth_legacy_entry(cfg);
    }
    const ModelProfile* by_name = find_by_name(cfg.saved_models, chosen_name);
    if (by_name != nullptr) return *by_name;
    return synth_legacy_entry(cfg);
}

} // namespace acecode
