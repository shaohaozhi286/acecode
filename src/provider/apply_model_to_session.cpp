// src/provider/apply_model_to_session.cpp
#include "apply_model_to_session.hpp"

#include "copilot_provider.hpp"
#include "model_context_resolver.hpp"
#include "model_pool_status.hpp"
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

SessionModelState state_from_profile(const AppConfig& cfg,
                                      const ModelProfile& profile) {
    SessionModelState state;
    state.name = profile.name;
    state.provider = profile.provider;
    state.model = profile.model;
    state.context_window = resolve_model_profile_context_window_nonblocking(
        cfg, profile, cfg.context_window);
    // model id 与监控快照的 modelPoolName 精确命中时,用 0.8 * maxWindowTokens
    // 作为有效上下文窗口。未命中或尚无数据时 eff==0,保留默认解析值。
    int eff = model_pool_status_service().effective_context_window_for(state.model);
    if (eff > 0) state.context_window = eff;
    return state;
}

} // namespace

ApplyModelResult apply_model_to_session(const ModelProfile& profile,
                                         const ApplyModelDeps& deps) {
    if (!deps.cfg) throw std::runtime_error("config unavailable");
    if (!deps.provider_slot) throw std::runtime_error("provider slot unavailable");

    ApplyModelResult result;
    result.state = state_from_profile(*deps.cfg, profile);

    auto new_provider = create_provider_from_entry(profile, deps.cfg);
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
