// routes_workspaces.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

void WebServer::Impl::register_workspaces() {
        CROW_ROUTE(app, "/api/workspaces").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/pick-folder").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/resume").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/archive").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/title").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/draft").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/todos").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });

        CROW_ROUTE(app, "/api/workspaces").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json arr = json::array();
            std::unordered_set<std::string> seen;
            if (deps.workspace_registry) {
                deps.workspace_registry->scan(projects_dir());
                for (const auto& m : deps.workspace_registry->list()) {
                    arr.push_back(workspace_to_json(m));
                    seen.insert(m.hash);
                }
            }
            auto compat = compatibility_workspace();
            if (!deps.workspace_registry && !compat.hash.empty() && !seen.count(compat.hash)) {
                arr.push_back(workspace_to_json(compat));
            }
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.workspace_registry) {
                crow::response r(503);
                r.body = R"({"error":"workspace registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            std::string cwd;
            try {
                auto j = json::parse(req.body);
                cwd = j.value("cwd", std::string{});
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (cwd.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto m = deps.workspace_registry->register_new(projects_dir(), cwd);
            LOG_INFO("[web] workspace registered hash=" + m.hash + " cwd=" + m.cwd);
            crow::response r(201);
            r.body = workspace_to_json(m).dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/pick-folder").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.native_folder_picker_enabled) {
                crow::response r(501);
                r.body = R"({"error":"native folder picker unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!deps.workspace_registry) {
                crow::response r(503);
                r.body = R"({"error":"workspace registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!deps.native_folder_picker) {
                crow::response r(503);
                r.body = R"({"error":"native folder picker callback unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto picked = deps.native_folder_picker();
            if (!picked || picked->empty()) {
                crow::response r(200);
                r.body = "null";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            std::string cwd = *picked;
            for (auto& c : cwd) {
                if (c == '\\') c = '/';
            }
            auto m = deps.workspace_registry->register_new(projects_dir(), cwd);
            LOG_INFO("[web] native folder picker registered workspace hash=" + m.hash + " cwd=" + m.cwd);
            crow::response r(200);
            r.body = workspace_to_json(m).dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // webapp 兼容模式(Edge --app,无 webview bridge)的「在资源管理器中打开」。
        // 路径校验(绝对路径 / 目录存在 / 在已注册 workspace 内)在回调内完成
        // (desktop::open_directory_in_file_manager),这里只做形参与门控。
        CROW_ROUTE(app, "/api/open-in-explorer").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.open_in_explorer) {
                crow::response r(501);
                r.body = R"({"error":"open in explorer unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            std::string path;
            try {
                auto j = json::parse(req.body);
                path = j.value("path", std::string{});
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (path.empty()) {
                crow::response r(400);
                r.body = R"({"error":"path required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto error = deps.open_in_explorer(path);
            if (error.has_value()) {
                crow::response r(400);
                r.body = json{{"ok", false}, {"error", *error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            LOG_INFO("[web] open-in-explorer path=" + path);
            crow::response r(200);
            r.body = R"({"ok":true})";
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& hash) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto arr = sessions_for_workspace(*ws, archived_query_requested(req));
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& hash) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                crow::response r(503);
                r.body = R"({"error":"session client unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!cwd_is_directory(ws->cwd)) {
                crow::response r(409);
                r.body = json{{"error", "workspace path unavailable"}, {"cwd", ws->cwd}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            SessionOptions opts;
            if (auto err = parse_session_options(req, *ws, opts)) return std::move(*err);
            refresh_default_session_preferences_for_new_session();
            auto id = deps.session_client->create_session(opts);
            LOG_INFO("[web] workspace session created hash=" + ws->hash + " id=" + id);
            crow::response r(201);
            r.body = json{{"session_id", id}, {"id", id}, {"workspace_hash", ws->hash}, {"cwd", ws->cwd}}.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/resume").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                crow::response r(503);
                r.body = R"({"error":"session client unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!cwd_is_directory(ws->cwd)) {
                crow::response r(409);
                r.body = json{{"error", "workspace path unavailable"}, {"cwd", ws->cwd}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            SessionOptions opts;
            opts.cwd = ws->cwd;
            opts.workspace_hash = ws->hash;
            auto project_dir = SessionStorage::get_project_dir(ws->cwd);
            if (auto lease = SessionWriterLease::read(project_dir, id)) {
                const bool other_pid = lease->pid != 0 && lease->pid != daemon::current_pid();
                const auto age_ms = SessionWriterLease::now_ms() - lease->updated_at_ms;
                const bool fresh = lease->updated_at_ms > 0 &&
                                   age_ms >= 0 &&
                                   age_ms <= SessionWriterLease::kDefaultStaleMs;
                if (other_pid && fresh && daemon::is_pid_alive(lease->pid)) {
                    crow::response r(409);
                    r.body = json{
                        {"error", "session already active"},
                        {"pid", lease->pid},
                        {"surface", lease->surface}
                    }.dump();
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
            }
            if (!deps.session_client->resume_session(id, opts)) {
                if (SessionStorage::has_incompatible_pid_session_files(project_dir, id)) {
                    crow::response r(409);
                    r.body = json{
                        {"error", "old session data incompatible"},
                        {"message", "PID-suffixed session data is no longer supported. Delete the old project session data under ~/.acecode/projects and start a new session."}
                    }.dump();
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                crow::response r(404);
                r.body = R"({"error":"session not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            LOG_INFO("[web] workspace session resumed hash=" + ws->hash + " id=" + id);
            crow::response r(200);
            r.body = json{{"session_id", id}, {"id", id}, {"active", true}, {"workspace_hash", ws->hash}, {"cwd", ws->cwd}}.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/archive").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            return set_session_archive_state(req, *ws, id, true);
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/archive").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            return set_session_archive_state(req, *ws, id, false);
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/title").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            return set_session_title_response(req, *ws, id);
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/draft").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            return get_session_input_draft(req, *ws, id);
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/draft").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            return set_session_input_draft(req, *ws, id);
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/todos").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            return clear_session_todos(req, *ws, id);
        });
    }

void WebServer::Impl::register_pinned_sessions() {
        CROW_ROUTE(app, "/api/workspaces/<string>/pinned-sessions").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/pinned-sessions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& hash) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const auto path = pinned_sessions_path_for_cwd(ws->cwd);
            auto state = read_pinned_sessions_state(path);
            const auto pruned = prune_pinned_session_ids(
                state.session_ids, session_ids_for_workspace(*ws));
            if (pruned != state.session_ids) {
                std::string ignored;
                write_pinned_sessions_state(path, PinnedSessionsState{pruned}, &ignored);
            }

            crow::response r(pinned_sessions_to_json(*ws, pruned).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/pinned-sessions").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& hash) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            std::vector<std::string> ids;
            try {
                auto body = json::parse(req.body.empty() ? "{}" : req.body);
                if (!body.contains("session_ids") || !body["session_ids"].is_array()) {
                    crow::response r(400);
                    r.body = R"({"error":"session_ids array required"})";
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                for (const auto& item : body["session_ids"]) {
                    if (item.is_string()) ids.push_back(item.get<std::string>());
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const auto next = prune_pinned_session_ids(
                normalize_pinned_session_ids(ids), session_ids_for_workspace(*ws));
            std::string error;
            if (!write_pinned_sessions_state(pinned_sessions_path_for_cwd(ws->cwd),
                                             PinnedSessionsState{next}, &error)) {
                crow::response r(500);
                r.body = json{{"error", "failed to write pinned sessions"},
                              {"detail", error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(pinned_sessions_to_json(*ws, next).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
    }

} // namespace acecode::web
