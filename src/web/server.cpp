// server.cpp — WebServer orchestrator.
// All helper implementations are in server_helpers.cpp.
// All register_*() method bodies are in routes/routes_*.cpp.
// This file contains only: Impl state definition, register_routes(),
// WebServer public methods (ctor, dtor, run, stop).

#include "server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

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
    register_ui_preferences();
    register_history();
    register_files();
    register_skills();
    register_commands();
    register_mcp();
    register_hooks();
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

} // namespace acecode::web
