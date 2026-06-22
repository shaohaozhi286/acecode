// routes_pty.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

// =====================================================================
// PTY-specific helpers
// =====================================================================

std::optional<crow::response> WebServer::Impl::require_pty_access(const crow::request& req) {
    if (!deps.pty_registry) {
        crow::response r(503);
        r.add_header("Content-Type", "application/json");
        r.body = json{{"error", "console is not available"}}.dump();
        add_cors(req, r);
        return r;
    }
    if (!is_loopback_address(req.remote_ip_address)) {
        log_unauthorized(req.url, req.remote_ip_address, "pty non-loopback");
        crow::response r(403);
        r.add_header("Content-Type", "application/json");
        r.body = json{{"error", "console is loopback-only"}}.dump();
        add_cors(req, r);
        return r;
    }
    return require_auth(req);
}

static json pty_info_json(const PtySessionInfo& info) {
    json j{
        {"id", info.id},
        {"title", info.title},
        {"shell", info.shell},
        {"cwd", info.cwd},
        {"status", info.status},
        {"pid", info.pid},
        {"backend", pty_backend_kind_name(info.backend)},
    };
    if (info.status == "exited") j["exit_code"] = info.exit_code;
    return j;
}

json WebServer::Impl::console_shells_payload() {
    json arr = json::array();
    const std::string git_bash_path =
        deps.app_config ? deps.app_config->console.git_bash_path : std::string{};
    const std::string default_id = deps.app_config
        ? default_console_shell_id(deps.app_config->console.default_shell, git_bash_path)
        : std::string{};
    for (const auto& opt : detect_console_shells(git_bash_path)) {
        arr.push_back({{"id", opt.id},
                       {"label", opt.label},
                       {"available", opt.available},
                       {"needs_path", opt.needs_path}});
    }
    return json{{"shells", arr}, {"default", default_id}};
}

struct PtyWsState {
    std::string id;
    std::int64_t cursor = -1;
};

void WebServer::Impl::register_pty() {
        CROW_ROUTE(app, "/api/pty").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/<string>").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/<string>/resize").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/<string>/title").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/shells").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/console/config").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });

        // GET /api/pty/shells → { shells:[{id,label,available,needs_path}], default }
        CROW_ROUTE(app, "/api/pty/shells").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            crow::response r(console_shells_payload().dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/pty {cwd?, title?, shell?} → 201 session info
        // shell = shell id(powershell/git-bash/cmd/...);省略 → 默认。id 不可用
        // (git-bash 需指定路径)→ 400 {error, shell, needs_path}。
        CROW_ROUTE(app, "/api/pty").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            std::string cwd_override, title, shell_id;
            if (!req.body.empty()) {
                try {
                    auto body = json::parse(req.body);
                    cwd_override = body.value("cwd", "");
                    title = body.value("title", "");
                    shell_id = body.value("shell", "");
                } catch (...) {
                    return with_cors(req, crow::response(400, "bad json"));
                }
            }
            std::string shell_override;
            if (!shell_id.empty()) {
                std::string git_bash_path;
                if (deps.app_config) {
                    std::lock_guard<std::mutex> config_lock(app_config_mu);
                    git_bash_path = deps.app_config->console.git_bash_path;
                }
                auto cmd = resolve_shell_command_by_id(shell_id, git_bash_path);
                if (!cmd) {
                    json e{{"error", "shell unavailable"}, {"shell", shell_id}};
                    if (shell_id == "git-bash") e["needs_path"] = true;
                    crow::response r(400, e.dump());
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                shell_override = *cmd;
            }
            std::string error;
            auto info = deps.pty_registry->create(cwd_override, title, shell_override, error);
            if (!info) {
                int code = error.find("limit") != std::string::npos ? 429 : 500;
                crow::response r(code);
                r.add_header("Content-Type", "application/json");
                r.body = json{{"error", error}}.dump();
                return with_cors(req, std::move(r));
            }
            crow::response r(201, pty_info_json(*info).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/pty → {backend, sessions: [...]}
        CROW_ROUTE(app, "/api/pty").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            json arr = json::array();
            for (const auto& info : deps.pty_registry->list()) {
                arr.push_back(pty_info_json(info));
            }
            json out{{"backend", pty_backend_kind_name(deps.pty_registry->backend())},
                     {"sessions", arr}};
            crow::response r(out.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // DELETE /api/pty/<id> → 204 / 404
        CROW_ROUTE(app, "/api/pty/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            if (!deps.pty_registry->remove(id)) {
                return with_cors(req, crow::response(404));
            }
            return with_cors(req, crow::response(204));
        });

        // POST /api/pty/<id>/resize {cols, rows} → 204 / 404
        CROW_ROUTE(app, "/api/pty/<string>/resize").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            int cols = 0, rows = 0;
            try {
                auto body = json::parse(req.body);
                cols = body.value("cols", 0);
                rows = body.value("rows", 0);
            } catch (...) {
                return with_cors(req, crow::response(400, "bad json"));
            }
            if (cols < 2 || cols > 1000 || rows < 2 || rows > 1000) {
                return with_cors(req, crow::response(400, "cols/rows out of range"));
            }
            if (!deps.pty_registry->resize(id, cols, rows)) {
                return with_cors(req, crow::response(404));
            }
            return with_cors(req, crow::response(204));
        });

        // POST /api/pty/<id>/title {title} → 204 / 404。终端内程序经 OSC
        // 设置的标题由前端 xterm onTitleChange 同步回来,刷新恢复不丢。
        CROW_ROUTE(app, "/api/pty/<string>/title").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            std::string title;
            try {
                auto body = json::parse(req.body);
                title = body.value("title", "");
            } catch (...) {
                return with_cors(req, crow::response(400, "bad json"));
            }
            if (!deps.pty_registry->set_title(id, title)) {
                return with_cors(req, crow::response(404));
            }
            return with_cors(req, crow::response(204));
        });

        // PUT /api/console/config {default_shell?, git_bash_path?} → 校验 + 持久化
        // (save_config 原子写,失败回滚)→ 返回刷新后的 shells payload。
        CROW_ROUTE(app, "/api/console/config").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            auto json_err = [&](int status, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };
            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, std::string("bad json: ") + e.what());
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            const std::string prev_default = deps.app_config->console.default_shell;
            const std::string prev_bash = deps.app_config->console.git_bash_path;

            if (body.contains("git_bash_path")) {
                if (!body["git_bash_path"].is_string()) {
                    return json_err(400, "git_bash_path must be a string");
                }
                std::string p = body["git_bash_path"].get<std::string>();
                // 去掉首尾空白与成对双引号(用户常粘进带引号的路径)。
                auto trim = [](std::string& s) {
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                                          s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                                          s.back() == '\r' || s.back() == '\n')) s.pop_back();
                };
                trim(p);
                if (p.size() >= 2 && p.front() == '"' && p.back() == '"') {
                    p = p.substr(1, p.size() - 2);
                    trim(p);
                }
                if (!p.empty()) {
                    if (is_wsl_system32_bash(p)) {
                        return json_err(400, "that looks like WSL bash (System32), not Git Bash");
                    }
                    std::error_code ec;
                    auto fs_path = std::filesystem::u8path(p);
                    if (!std::filesystem::exists(fs_path, ec) ||
                            std::filesystem::is_directory(fs_path, ec)) {
                        return json_err(400, "bash path does not exist");
                    }
                }
                deps.app_config->console.git_bash_path = p;
            }
            if (body.contains("default_shell")) {
                if (!body["default_shell"].is_string()) {
                    return json_err(400, "default_shell must be a string");
                }
                deps.app_config->console.default_shell = body["default_shell"].get<std::string>();
            }

            try {
                if (!deps.config_path.empty()) save_config(*deps.app_config, deps.config_path);
                else save_config(*deps.app_config);
            } catch (const std::exception& e) {
                deps.app_config->console.default_shell = prev_default;
                deps.app_config->console.git_bash_path = prev_bash;
                return json_err(500, std::string("persist failed: ") + e.what());
            }

            crow::response r(200, console_shells_payload().dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // WS /ws/pty/<id>?cursor=N — 原始字节直传 + 0x00 控制帧。
        CROW_WEBSOCKET_ROUTE(app, "/ws/pty/<string>")
            .onaccept([this](const crow::request& req, void** userdata) -> bool {
                if (!deps.pty_registry) return false;
                if (!is_loopback_address(req.remote_ip_address)) {
                    log_unauthorized(req.url, req.remote_ip_address, "pty ws non-loopback");
                    return false;
                }
                // Crow 的 WS onopen 拿不到 route 参数,从 url 解出 id 存
                // userdata("/ws/pty/<id>",query 已被 Crow 剥离到 url_params)。
                std::string path = req.url;
                const std::string prefix = "/ws/pty/";
                auto pos = path.find(prefix);
                if (pos == std::string::npos) return false;
                std::string id = path.substr(pos + prefix.size());
                if (auto qpos = id.find('?'); qpos != std::string::npos) {
                    id = id.substr(0, qpos);
                }
                if (id.empty()) return false;
                auto* state = new PtyWsState();
                state->id = id;
                if (auto c = req.url_params.get("cursor")) {
                    try { state->cursor = std::stoll(c); } catch (...) {}
                }
                *userdata = state;
                return true;
            })
            .onopen([this](crow::websocket::connection& conn) {
                auto* state = static_cast<PtyWsState*>(conn.userdata());
                if (!state) { conn.close("no state"); return; }
                bool ok = deps.pty_registry->connect(
                    state->id, &conn, state->cursor,
                    [&conn](const std::string& frame) {
                        conn.send_binary(frame);
                    });
                if (!ok) {
                    conn.close("unknown pty session");
                }
            })
            .onmessage([this](crow::websocket::connection& conn,
                              const std::string& data, bool /*is_binary*/) {
                auto* state = static_cast<PtyWsState*>(conn.userdata());
                if (!state) return;
                deps.pty_registry->write_input(state->id, data);
            })
            .onclose([this](crow::websocket::connection& conn,
                            const std::string& /*reason*/, uint16_t /*code*/) {
                auto* state = static_cast<PtyWsState*>(conn.userdata());
                if (!state) return;
                if (deps.pty_registry) {
                    deps.pty_registry->disconnect(state->id, &conn);
                }
                conn.userdata(nullptr);
                delete state;
            });
    }
} // namespace acecode::web
