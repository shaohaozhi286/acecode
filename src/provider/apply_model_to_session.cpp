// src/provider/apply_model_to_session.cpp
#include "apply_model_to_session.hpp"

#include "copilot_provider.hpp"
#include "model_context_resolver.hpp"
#include "model_resolver.hpp"
#include "provider_factory.hpp"
#include "../agent_loop.hpp"
#include "../session/session_manager.hpp"
#include "../utils/logger.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace acecode {

namespace {

AppConfig config_for_profile_context(const AppConfig& cfg,
                                     const ModelProfile& profile) {
    AppConfig c = cfg;
    c.provider = profile.provider;
    if (profile.provider == "openai") {
        c.openai.base_url = profile.base_url;
        c.openai.api_key = profile.api_key;
        c.openai.model = profile.model;
        c.openai.models_dev_provider_id = profile.models_dev_provider_id;
    } else {
        c.copilot.model = profile.model;
    }
    return c;
}

SessionModelState state_from_profile(const AppConfig& cfg,
                                      const ModelProfile& profile) {
    auto context_cfg = config_for_profile_context(cfg, profile);
    SessionModelState state;
    state.name = profile.name;
    state.provider = profile.provider;
    state.model = profile.model;
    state.context_window = resolve_model_context_window(
        context_cfg, profile.provider, profile.model, cfg.context_window);
    state.is_legacy = profile.name == "(legacy)";
    return state;
}

} // namespace

ApplyModelResult apply_model_to_session(const ModelProfile& profile,
                                         const ApplyModelDeps& deps) {
    if (!deps.cfg) throw std::runtime_error("config unavailable");
    if (!deps.provider_slot) throw std::runtime_error("provider slot unavailable");

    ApplyModelResult result;
    result.state = state_from_profile(*deps.cfg, profile);

    auto new_provider = create_provider_from_entry(profile);
    if (!new_provider) {
        throw std::runtime_error("provider create failed: factory returned null for '"
                                 + profile.name + "'");
    }

    if (new_provider->name() == "copilot") {
        if (auto cp = std::dynamic_pointer_cast<CopilotProvider>(new_provider)) {
            if (!cp->try_silent_auth()) {
                result.warning = "Copilot silent_auth failed; user may need /login "
                                 "before next request";
                LOG_WARN("[apply_model_to_session] " + result.warning);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(deps.provider_slot->mu);
        deps.provider_slot->provider = std::move(new_provider);
    }

    if (deps.loop && result.state.context_window > 0) {
        deps.loop->set_context_window(result.state.context_window);
    }

    if (deps.sm) {
        try {
            deps.sm->set_active_provider(result.state.provider,
                                          result.state.model,
                                          result.state.name);
        } catch (const std::exception& e) {
            // meta 写盘失败是非致命的:slot 已替换,用户下一发用新模型;
            // 唯一后果是 daemon 崩了恢复时显示旧模型,用户重切一次即可。
            std::string msg = std::string("meta persist failed: ") + e.what();
            if (result.warning.empty()) result.warning = msg;
            else result.warning += "; " + msg;
            LOG_WARN("[apply_model_to_session] " + msg);
        }
    }

    LOG_INFO("[apply_model_to_session] applied entry='" + profile.name +
             "' (" + result.state.provider + "/" + result.state.model +
             "), context_window=" + std::to_string(result.state.context_window));
    return result;
}

} // namespace acecode
