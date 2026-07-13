// server.cpp — WebServer orchestrator.
// All helper implementations are in server_helpers.cpp.
// All register_*() method bodies are in routes/routes_*.cpp.
// This file contains only: Impl state definition, register_routes(),
// WebServer public methods (ctor, dtor, run, stop).

#include "server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

WebServer::Impl::~Impl() {
    if (subagent_tracker_state) {
        std::lock_guard<std::mutex> lk(subagent_tracker_state->mu);
        subagent_tracker_state->impl = nullptr;
    }

    std::vector<std::pair<std::string, SessionClient::SubscriptionId>> subs;
    {
        std::lock_guard<std::mutex> lk(tracked_subagents_mu);
        subs.reserve(tracked_subagent_subscriptions.size());
        for (const auto& [sid, sub] : tracked_subagent_subscriptions) {
            subs.emplace_back(sid, sub);
        }
        tracked_subagent_subscriptions.clear();
    }
    if (deps.session_client) {
        for (const auto& [sid, sub] : subs) {
            deps.session_client->unsubscribe(sid, sub);
        }
    }
}

// =====================================================================
// register_routes — dispatches to each domain's register_*()
// =====================================================================
void WebServer::Impl::register_routes() {
    register_health();
    register_usage();
    register_workspaces();
    register_pinned_sessions();
    register_sessions();
    register_models();
    register_loops();
    register_ui_preferences();
    register_history();
    register_files();
    register_git();
    register_lsp();
    register_skills();
    register_commands();
    register_mcp();
    register_hooks();
    register_feedback();
    register_pty();
    register_websocket();
    register_static();
}

// =====================================================================
// WebServer public methods
// =====================================================================
WebServer::WebServer(WebServerDeps deps)
    : impl_(std::make_unique<Impl>(std::move(deps))) {
    try {
        std::string dir = impl_->deps.web_cfg ? impl_->deps.web_cfg->static_dir : std::string{};
        impl_->assets = make_asset_source(dir);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[web] failed to init asset source: ") + e.what());
    }
    impl_->register_routes();
}

WebServer::~WebServer() = default;

int WebServer::run() {
    if (!impl_->deps.web_cfg) {
        LOG_ERROR("[web] missing web_cfg");
        return 1;
    }
    const auto& cfg = *impl_->deps.web_cfg;

    auto preflight = preflight_bind_check(cfg.bind, impl_->deps.token, impl_->deps.dangerous);
    if (!preflight.empty()) {
        LOG_ERROR("[web] " + preflight);
        return 2;
    }

    LOG_INFO("[web] listening on " + cfg.bind + ":" + std::to_string(cfg.port));
    try {
        impl_->app
            .bindaddr(cfg.bind)
            .port(static_cast<std::uint16_t>(cfg.port))
            .multithreaded()
            .run();
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[web] server crashed: ") + e.what());
        LOG_ERROR("[web] port " + std::to_string(cfg.port) +
                  " may be in use — change web.port in config.json or stop "
                  "the conflicting process; daemon will not retry");
        return 3;
    }
    return 0;
}

void WebServer::stop() {
    if (impl_) impl_->app.stop();
}

void WebServer::track_subagent(const std::string& child_session_id) {
    if (impl_) impl_->track_subagent(child_session_id);
}

void WebServer::refresh_saved_models_from_disk() {
    if (impl_) impl_->refresh_saved_models_from_disk();
}

void WebServer::with_app_config_lock(const std::function<void()>& fn) const {
    if (!fn) return;
    if (!impl_) {
        fn();
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->app_config_mu);
    fn();
}

} // namespace acecode::web
