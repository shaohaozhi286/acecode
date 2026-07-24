// routes_workspaces.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"
#include "../project_creation.hpp"

namespace acecode::web {

using nlohmann::json;

namespace {

json opencode_import_status_to_json(const OpencodeImportJobStatus& status) {
    return json{
        {"job_id", status.job_id},
        {"workspace_hash", status.workspace_hash},
        {"state", status.state},
        {"imported", status.imported},
        {"total", status.total},
        {"failed", status.failed},
        {"skipped", status.skipped},
        {"current_title", status.current_title},
        {"error", status.error},
        {"session_ids", status.session_ids},
    };
}

json opencode_import_preview_to_json(const OpencodeImportPreview& preview) {
    json sessions = json::array();
    for (const auto& session : preview.sessions) {
        sessions.push_back(json{
            {"id", session.opencode_session_id},
            {"title", session.title},
            {"directory", session.directory},
            {"provider", session.provider},
            {"model", session.model},
            {"archived", session.archived},
            {"time_created_ms", session.time_created_ms},
            {"time_updated_ms", session.time_updated_ms},
            {"time_archived_ms", session.time_archived_ms},
            {"message_count", session.message_count},
            {"part_count", session.part_count},
            {"source_database", session.source_database},
        });
    }
    return json{
        {"available", preview.available},
        {"count", preview.count},
        {"source_database", preview.source_database},
        {"error", preview.error},
        {"sessions", sessions},
    };
}

int project_creation_http_status(ProjectCreationError error) {
    switch (error) {
        case ProjectCreationError::NameRequired:
        case ProjectCreationError::ParentMustBeAbsolute:
        case ProjectCreationError::ParentNotFound:
        case ProjectCreationError::ParentNotDirectory:
            return 400;
        case ProjectCreationError::TargetExists:
            return 409;
        case ProjectCreationError::CreateFailed:
            return 500;
        case ProjectCreationError::None:
            return 200;
    }
    return 500;
}

} // namespace

void WebServer::Impl::register_workspaces() {
        CROW_ROUTE(app, "/api/workspaces").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/pick-folder").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/projects/defaults").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/projects").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/opencode-import").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/opencode-import/<string>").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>/resume").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>").methods(crow::HTTPMethod::Options)
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

        CROW_ROUTE(app, "/api/projects/defaults").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            crow::response r(json{
                {"parent_dir", default_project_parent_directory(projects_dir())},
            }.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/projects").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.workspace_registry) {
                crow::response r(503);
                r.body = R"({"error":"workspace registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            std::string name;
            std::string parent_dir;
            try {
                const auto body = json::parse(req.body);
                if (!body.is_object()) throw std::runtime_error("object required");
                if (auto it = body.find("name"); it != body.end()) {
                    if (!it->is_string()) throw std::runtime_error("name must be a string");
                    name = it->get<std::string>();
                }
                if (auto it = body.find("parent_dir"); it != body.end() && !it->is_null()) {
                    if (!it->is_string()) {
                        throw std::runtime_error("parent_dir must be a string");
                    }
                    parent_dir = it->get<std::string>();
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", "PROJECT_BAD_REQUEST"},
                              {"message", std::string("请求格式错误：") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto created = create_project_directory(name, parent_dir, projects_dir());
            if (!created.ok()) {
                crow::response r(project_creation_http_status(created.error));
                r.body = json{
                    {"error", project_creation_error_code(created.error)},
                    {"message", created.message},
                    {"directory_name", created.directory_name},
                    {"parent_dir", created.parent_dir},
                    {"project_dir", created.project_dir},
                    {"sanitized", created.sanitized},
                }.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto workspace = deps.workspace_registry->register_new(
                projects_dir(), created.project_dir);
            json body = workspace_to_json(workspace);
            body["requested_name"] = created.requested_name;
            body["directory_name"] = created.directory_name;
            body["parent_dir"] = created.parent_dir;
            body["project_dir"] = created.project_dir;
            body["sanitized"] = created.sanitized;
            LOG_INFO("[web] project created cwd=" + created.project_dir +
                     " workspace_hash=" + workspace.hash);
            crow::response r(201);
            r.body = body.dump();
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

        CROW_ROUTE(app, "/api/workspaces/<string>/opencode-import").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& hash) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            const std::string project_dir = SessionStorage::get_project_dir(ws->cwd);
            auto preview = preview_opencode_import(ws->cwd, project_dir);
            crow::response r(opencode_import_preview_to_json(preview).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/opencode-import").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& hash) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            OpencodeImportOptions opts;
            opts.workspace_hash = ws->hash;
            opts.cwd = ws->cwd;
            opts.project_dir = SessionStorage::get_project_dir(ws->cwd);
            try {
                const auto body = json::parse(req.body.empty() ? "{}" : req.body);
                if (auto it = body.find("session_ids"); it != body.end()) {
                    if (!it->is_array()) {
                        crow::response r(400);
                        r.body = R"({"error":"session_ids array required"})";
                        r.add_header("Content-Type", "application/json");
                        return with_cors(req, std::move(r));
                    }
                    opts.selected_session_ids_provided = true;
                    for (const auto& item : *it) {
                        if (!item.is_string()) {
                            crow::response r(400);
                            r.body = R"({"error":"session_ids must contain strings"})";
                            r.add_header("Content-Type", "application/json");
                            return with_cors(req, std::move(r));
                        }
                        opts.selected_session_ids.push_back(item.get<std::string>());
                    }
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const std::string job_id = SessionStorage::generate_session_id();
            OpencodeImportJobStatus initial;
            initial.job_id = job_id;
            initial.workspace_hash = ws->hash;
            initial.state = "pending";
            if (opts.selected_session_ids_provided) {
                initial.total = static_cast<int>(opts.selected_session_ids.size());
            }
            {
                std::lock_guard<std::mutex> lk(opencode_import_mu);
                opencode_import_jobs[job_id] = initial;
            }

            std::thread([this, job_id, opts]() {
                auto publish = [this, job_id](const OpencodeImportJobStatus& next) {
                    auto copy = next;
                    copy.job_id = job_id;
                    if (copy.workspace_hash.empty()) copy.workspace_hash = next.workspace_hash;
                    std::lock_guard<std::mutex> lk(opencode_import_mu);
                    opencode_import_jobs[job_id] = std::move(copy);
                };
                auto final_status = import_opencode_sessions(opts, publish);
                final_status.job_id = job_id;
                final_status.workspace_hash = opts.workspace_hash;
                publish(final_status);
            }).detach();

            crow::response r(202);
            r.body = opencode_import_status_to_json(initial).dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/workspaces/<string>/opencode-import/<string>").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& hash, const std::string& job_id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto ws = resolve_workspace(hash);
            if (!ws.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"workspace not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            std::lock_guard<std::mutex> lk(opencode_import_mu);
            auto it = opencode_import_jobs.find(job_id);
            if (it == opencode_import_jobs.end() || it->second.workspace_hash != ws->hash) {
                crow::response r(404);
                r.body = R"({"error":"import job not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            crow::response r(opencode_import_status_to_json(it->second).dump());
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
            const char* parent_raw = req.url_params.get("parent");
            auto arr = sessions_for_workspace(
                *ws, archived_query_requested(req),
                /*include_no_workspace=*/false,
                parent_raw ? std::string(parent_raw) : std::string{});
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
            opts.no_workspace = false;
            opts.workspace_hash = ws->hash;
            std::string id;
            try {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                refresh_default_session_preferences_for_new_session_locked();
                id = deps.session_client->create_session(opts);
            } catch (const std::invalid_argument& ex) {
                crow::response r(400);
                r.body = json{{"error", "INVALID_EXPERT"},
                              {"message", ex.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            LOG_INFO("[web] workspace session created hash=" + ws->hash + " id=" + id);
            crow::response r(201);
            r.body = json{{"session_id", id}, {"id", id}, {"workspace_hash", ws->hash},
                          {"cwd", ws->cwd}, {"expert_id", opts.expert_id}}.dump();
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
            auto meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, id));
            if (meta.no_workspace) {
                crow::response r(404);
                r.body = R"({"error":"session not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
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
            bool resumed = false;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                resumed = deps.session_client->resume_session(id, opts);
            }
            if (!resumed) {
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

        CROW_ROUTE(app, "/api/workspaces/<string>/sessions/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& hash, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            const char* purge_raw = req.url_params.get("purge");
            const bool purge = purge_raw &&
                (std::string(purge_raw) == "1" || std::string(purge_raw) == "true");
            if (!purge) {
                crow::response r(400);
                r.body = R"({"error":"purge=1 is required"})";
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
            return purge_session_data(req, *ws, id, /*require_archived=*/true);
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
        CROW_ROUTE(app, "/api/pinned-sessions/order").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        CROW_ROUTE(app, "/api/pinned-sessions/order").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            const auto path = pinned_session_order_path();
            auto state = read_pinned_session_order_state(path);
            const auto pruned = prune_pinned_session_order_items(
                state.items, available_pinned_session_order_items());
            if (pruned != state.items) {
                std::string ignored;
                write_pinned_session_order_state(path, PinnedSessionOrderState{pruned}, &ignored);
            }

            crow::response r(pinned_session_order_to_json(pruned).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/pinned-sessions/order").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::vector<PinnedSessionOrderItem> items;
            try {
                auto body = json::parse(req.body.empty() ? "{}" : req.body);
                if (!body.contains("items") || !body["items"].is_array()) {
                    crow::response r(400);
                    r.body = R"({"error":"items array required"})";
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                for (const auto& raw : body["items"]) {
                    if (!raw.is_object()) continue;
                    PinnedSessionOrderItem item;
                    if (raw.contains("workspace_hash") && raw["workspace_hash"].is_string()) {
                        item.workspace_hash = raw["workspace_hash"].get<std::string>();
                    }
                    if (raw.contains("session_id") && raw["session_id"].is_string()) {
                        item.session_id = raw["session_id"].get<std::string>();
                    }
                    items.push_back(std::move(item));
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const auto next = prune_pinned_session_order_items(
                normalize_pinned_session_order_items(items),
                available_pinned_session_order_items());
            std::string error;
            if (!write_pinned_session_order_state(pinned_session_order_path(),
                                                  PinnedSessionOrderState{next}, &error)) {
                crow::response r(500);
                r.body = json{{"error", "failed to write pinned session order"},
                              {"detail", error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(pinned_session_order_to_json(next).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

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
