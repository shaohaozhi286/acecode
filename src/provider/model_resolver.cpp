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

ModelProfile resolve_effective_model(const AppConfig& cfg,
                                   const std::optional<std::string>& cwd_override_name,
                                   const std::optional<SessionMeta>& resumed_meta) {
    // 第 1 层:global default
    std::string chosen_name = cfg.default_model_name;

    // 第 2 层:cwd override(非空字符串才算设了 override)
    if (cwd_override_name.has_value() && !cwd_override_name->empty()) {
        // 确认 override name 指向的 entry 存在;不存在时记 warning 并走第 1 层。
        if (find_by_name(cfg.saved_models, *cwd_override_name) != nullptr) {
            chosen_name = *cwd_override_name;
        } else {
            LOG_WARN("[model_resolver] cwd override points to missing entry '" +
                     *cwd_override_name + "' (from model_override.json); "
                     "falling back to default saved model");
        }
    }

    // 第 3 层:resume session meta —— 新 meta 优先保留明确的 preset name;
    // 老 meta 没有 name 时仍按 (provider, model) 查 saved_models。
    if (resumed_meta.has_value() &&
        !resumed_meta->provider.empty() &&
        !resumed_meta->model.empty()) {
        if (!resumed_meta->model_preset.empty()) {
            const ModelProfile* by_meta_name = find_by_name(
                cfg.saved_models, resumed_meta->model_preset);
            if (by_meta_name != nullptr) {
                return *by_meta_name;
            }
            LOG_WARN("[model_resolver] session meta points to missing model preset '" +
                     resumed_meta->model_preset + "'; falling back to provider/model match");
        }
        const ModelProfile* matched = find_by_provider_model(
            cfg.saved_models, resumed_meta->provider, resumed_meta->model);
        if (matched != nullptr) {
            return *matched;
        }
        // 未命中 —— ad-hoc 兜底(spec: name 以 "(session:" 开头,触发系统提示)。
        return build_ad_hoc_entry(cfg, *resumed_meta);
    }

    // 第 4 层:按 chosen_name 查 saved_models;没配置 default 时取 saved_models 首项。
    const ModelProfile* by_name = find_by_name(cfg.saved_models, chosen_name);
    if (by_name != nullptr) return *by_name;
    if (!cfg.saved_models.empty()) return cfg.saved_models.front();

    ModelProfile legacy = legacy_model_profile_from_config(cfg);
    LOG_WARN("[model_resolver] no saved_models configured; using legacy " +
             legacy.provider + "/" + legacy.model + " profile '" + legacy.name + "'");
    return legacy;
}

} // namespace acecode
