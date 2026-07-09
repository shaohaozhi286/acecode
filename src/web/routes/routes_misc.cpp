// routes_misc.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"
#include "../../feedback/feedback_upload.hpp"
#include "../../tool/mcp_manager.hpp"  // /api/mcp/toggle 运行时 enable/disable

namespace acecode::web {

namespace fs = std::filesystem;

using nlohmann::json;

namespace {

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string url_decode_component(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int hi = hex_value(value[i + 1]);
            int lo = hex_value(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i]);
    }
    return out;
}

} // namespace

void WebServer::Impl::register_hooks() {
        auto json_err = [this](const crow::request& req,
                               int status,
                               const char* code,
                               const std::string& msg) {
            crow::response r(status);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"error", code}, {"message", msg}}.dump();
            return with_cors(req, std::move(r));
        };

        auto load_store = [](std::string* error) {
            return acecode::load_hook_trust_store_from_path(
                acecode::default_hook_trust_state_path(), error);
        };

        auto reload_snapshot = [this](const acecode::HookTrustStore& store) {
            acecode::HookLoadOptions options;
            options.feature_enabled = deps.app_config ? deps.app_config->features.hooks : true;
            options.cwd = deps.cwd;
            options.project_trusted = true;
            return acecode::load_hook_registry(options, &store);
        };

        auto find_hook = [](const acecode::HookRegistrySnapshot& snapshot,
                            const std::string& hook_id) -> const acecode::NormalizedHook* {
            for (const auto& hook : snapshot.hooks) {
                if (hook.id == hook_id) return &hook;
            }
            return nullptr;
        };

        CROW_ROUTE(app, "/api/hooks").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/hooks/refresh").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/hooks/<string>/trust").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/hooks/<string>/disable").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/hooks/<string>/enable").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });

        CROW_ROUTE(app, "/api/hooks").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.hook_manager) return crow::response(503);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = acecode::hook_registry_snapshot_to_json(
                deps.hook_manager->registry_snapshot()).dump();
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/hooks/refresh").methods(crow::HTTPMethod::POST)
        ([this, load_store, reload_snapshot, json_err](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.hook_manager) return crow::response(503);
            std::string error;
            auto store = load_store(&error);
            if (!error.empty()) {
                LOG_WARN("[hooks] " + error);
            }
            auto snapshot = reload_snapshot(store);
            deps.hook_manager->refresh_registry(std::move(snapshot));
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = acecode::hook_registry_snapshot_to_json(
                deps.hook_manager->registry_snapshot()).dump();
            return with_cors(req, std::move(r));
        });

        auto mutate_hook = [this, load_store, reload_snapshot, find_hook, json_err](
            const crow::request& req,
            const std::string& hook_id,
            const std::string& action) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.hook_manager) return crow::response(503);

            const auto snapshot = deps.hook_manager->registry_snapshot();
            const auto decoded_hook_id = url_decode_component(hook_id);
            const auto* hook = find_hook(snapshot, decoded_hook_id);
            if (!hook) {
                return json_err(req, 404, "HOOK_NOT_FOUND", "hook not found");
            }
            if ((action == "disable" || action == "enable") &&
                (hook->managed || hook->trust_status == acecode::HookTrustStatus::ManagedTrusted)) {
                return json_err(req, 409, "HOOK_MANAGED", "managed hooks cannot be disabled");
            }

            std::string error;
            auto store = load_store(&error);
            if (!error.empty()) {
                LOG_WARN("[hooks] " + error);
            }

            const bool legacy_config_toggle =
                (action == "disable" || action == "enable") &&
                hook->legacy_direct &&
                hook->source_format == acecode::HookSourceFormat::AceCodeLegacyJson &&
                !hook->source_path.empty();

            if (action == "trust") {
                acecode::trust_hook_definition(store, *hook);
            } else if (action == "disable") {
                if (legacy_config_toggle) {
                    if (!acecode::set_hook_config_enabled_in_path(
                            hook->source_path, false, &error)) {
                        return json_err(req, 500, "HOOK_CONFIG_SAVE_FAILED", error);
                    }
                } else {
                    acecode::set_hook_disabled(store, *hook, true);
                }
            } else if (action == "enable") {
                acecode::set_hook_disabled(store, *hook, false);
            }

            if (!legacy_config_toggle || action == "enable") {
                if (!acecode::save_hook_trust_store_to_path(
                        store, acecode::default_hook_trust_state_path(), &error)) {
                    return json_err(req, 500, "HOOK_STATE_SAVE_FAILED", error);
                }
            }

            if (legacy_config_toggle && action == "enable" &&
                !acecode::set_hook_config_enabled_in_path(
                    hook->source_path, true, &error)) {
                return json_err(req, 500, "HOOK_CONFIG_SAVE_FAILED", error);
            }

            auto reloaded = reload_snapshot(store);
            deps.hook_manager->refresh_registry(std::move(reloaded));
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = acecode::hook_registry_snapshot_to_json(
                deps.hook_manager->registry_snapshot()).dump();
            return with_cors(req, std::move(r));
        };

        CROW_ROUTE(app, "/api/hooks/<string>/trust").methods(crow::HTTPMethod::POST)
        ([mutate_hook](const crow::request& req, const std::string& hook_id) {
            return mutate_hook(req, hook_id, "trust");
        });
        CROW_ROUTE(app, "/api/hooks/<string>/disable").methods(crow::HTTPMethod::POST)
        ([mutate_hook](const crow::request& req, const std::string& hook_id) {
            return mutate_hook(req, hook_id, "disable");
        });
        CROW_ROUTE(app, "/api/hooks/<string>/enable").methods(crow::HTTPMethod::POST)
        ([mutate_hook](const crow::request& req, const std::string& hook_id) {
            return mutate_hook(req, hook_id, "enable");
        });
    }

void WebServer::Impl::register_feedback() {
        auto json_err = [this](const crow::request& req,
                               int status,
                               const char* code,
                               const std::string& msg,
                               json extra = json::object()) {
            crow::response r(status);
            r.add_header("Content-Type", "application/json");
            json body = {{"error", code}, {"message", msg}};
            if (extra.is_object()) {
                for (auto it = extra.begin(); it != extra.end(); ++it) {
                    body[it.key()] = it.value();
                }
            }
            r.body = body.dump();
            return with_cors(req, std::move(r));
        };

        auto candidate_workspaces = [this]() {
            std::vector<acecode::desktop::WorkspaceMeta> workspaces;
            std::unordered_set<std::string> seen;
            auto add = [&](const acecode::desktop::WorkspaceMeta& ws) {
                const std::string key = ws.hash.empty() ? ws.cwd : ws.hash;
                if (key.empty() || seen.count(key)) return;
                seen.insert(key);
                workspaces.push_back(ws);
            };

            add(compatibility_workspace());
            if (deps.workspace_registry) {
                deps.workspace_registry->scan(projects_dir());
                for (const auto& ws : deps.workspace_registry->list()) {
                    add(ws);
                }
            }
            return workspaces;
        };

        auto session_sort_key = [](const json& item) {
            std::string key = item.value("updated_at", std::string{});
            if (key.empty()) key = item.value("created_at", std::string{});
            return key;
        };

        auto find_selected_session = [this, candidate_workspaces](
            const std::string& session_id,
            const std::string& workspace_hash,
            acecode::desktop::WorkspaceMeta* out_ws,
            SessionMeta* out_meta,
            fs::path* out_jsonl) -> std::optional<std::string> {
            if (session_id.empty()) return std::string{"missing session id"};

            std::vector<acecode::desktop::WorkspaceMeta> workspaces;
            if (!workspace_hash.empty()) {
                auto ws = resolve_workspace(workspace_hash);
                if (!ws) return std::string{"workspace not found"};
                workspaces.push_back(*ws);
            } else {
                workspaces = candidate_workspaces();
            }

            struct Match {
                acecode::desktop::WorkspaceMeta ws;
                SessionMeta meta;
                fs::path jsonl;
            };
            std::vector<Match> matches;
            for (const auto& ws : workspaces) {
                const std::string project_dir = SessionStorage::get_project_dir(ws.cwd);
                auto candidates = SessionStorage::find_session_files(project_dir, session_id);
                if (candidates.empty() || candidates.front().jsonl_path.empty()) continue;

                SessionMeta meta = SessionStorage::read_meta(
                    candidates.front().meta_path.empty()
                        ? SessionStorage::meta_path(project_dir, session_id)
                        : candidates.front().meta_path);
                if (meta.id.empty()) meta.id = session_id;
                if (!workspace_hash.empty() && meta.no_workspace) {
                    return std::string{"session does not belong to requested workspace"};
                }
                matches.push_back(Match{
                    ws,
                    std::move(meta),
                    path_from_utf8(candidates.front().jsonl_path),
                });
            }

            if (matches.empty()) return std::string{"session JSONL not found"};
            if (matches.size() > 1) {
                return std::string{
                    "session id exists in multiple workspaces; workspace_hash is required"};
            }
            *out_ws = matches.front().ws;
            *out_meta = std::move(matches.front().meta);
            *out_jsonl = std::move(matches.front().jsonl);
            return std::nullopt;
        };

        CROW_ROUTE(app, "/api/feedback/desktop/recent-sessions").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/feedback/desktop").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        CROW_ROUTE(app, "/api/feedback/desktop/recent-sessions").methods(crow::HTTPMethod::GET)
        ([this, candidate_workspaces, session_sort_key](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            int limit = 20;
            if (auto raw = req.url_params.get("limit")) {
                try {
                    limit = std::clamp(std::stoi(raw), 1, 100);
                } catch (...) {
                    limit = 20;
                }
            }

            std::vector<json> sessions;
            std::unordered_set<std::string> seen;
            for (const auto& ws : candidate_workspaces()) {
                auto arr = sessions_for_workspace(ws, /*archived_only=*/false,
                                                  /*include_no_workspace=*/true);
                if (!arr.is_array()) continue;
                for (auto item : arr) {
                    const std::string id = item.value("id", item.value("session_id", std::string{}));
                    if (id.empty()) continue;
                    const std::string hash = item.value("workspace_hash", std::string{});
                    const std::string key = hash + "::" + id;
                    if (seen.count(key)) continue;
                    seen.insert(key);
                    if (!item.contains("session_id")) item["session_id"] = id;
                    sessions.push_back(std::move(item));
                }
            }

            std::sort(sessions.begin(), sessions.end(),
                      [session_sort_key](const json& a, const json& b) {
                          return session_sort_key(a) > session_sort_key(b);
                      });
            if (sessions.size() > static_cast<std::size_t>(limit)) {
                sessions.resize(static_cast<std::size_t>(limit));
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"sessions", sessions}}.dump();
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/feedback/desktop").methods(crow::HTTPMethod::POST)
        ([this, find_selected_session, json_err](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            json body = json::object();
            if (!req.body.empty()) {
                try { body = json::parse(req.body); }
                catch (const std::exception& e) {
                    return json_err(req, 400, "BAD_JSON",
                                    std::string("invalid JSON body: ") + e.what());
                }
                if (!body.is_object()) {
                    return json_err(req, 400, "BAD_REQUEST", "expected JSON object");
                }
            }

            auto optional_string = [&](const char* key) -> std::optional<std::string> {
                if (!body.contains(key) || body[key].is_null()) return std::string{};
                if (!body[key].is_string()) return std::nullopt;
                return body[key].get<std::string>();
            };
            auto feedback_text = optional_string("feedback_text");
            auto session_id = optional_string("session_id");
            auto workspace_hash = optional_string("workspace_hash");
            if (!feedback_text || !session_id || !workspace_hash) {
                return json_err(req, 400, "BAD_REQUEST",
                                "expected string feedback_text, session_id, and workspace_hash fields");
            }

            UpgradeConfig upgrade_cfg;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                upgrade_cfg = deps.app_config->upgrade;
            }
            const std::string upload_url = normalize_upgrade_base_url(upgrade_cfg.base_url);
            if (!is_valid_upgrade_base_url(upload_url)) {
                return json_err(req, 400, "BAD_REQUEST",
                                "upgrade.base_url must be a non-empty http or https URL");
            }

            acecode::feedback::FeedbackPackageRequest package_req;
            package_req.source = "desktop";
            package_req.feedback_text = *feedback_text;
            package_req.acecode_version = ACECODE_VERSION;
            package_req.log_entry_name = "logs/desktop.log.tail.txt";
            if (!deps.feedback_output_dir.empty()) {
                package_req.output_dir = path_from_utf8(deps.feedback_output_dir);
            }

            const fs::path logs_dir = deps.logs_dir.empty()
                ? path_from_utf8(get_logs_dir())
                : path_from_utf8(deps.logs_dir);
            if (auto log_path = acecode::feedback::latest_desktop_log_path(logs_dir)) {
                package_req.log_path = *log_path;
            }

            std::string selected_session_id = *session_id;
            std::string selected_workspace_hash = *workspace_hash;
            if (!selected_session_id.empty()) {
                acecode::desktop::WorkspaceMeta ws;
                SessionMeta meta;
                fs::path session_jsonl;
                if (auto error = find_selected_session(
                        selected_session_id, selected_workspace_hash, &ws, &meta, &session_jsonl)) {
                    return json_err(req, 404, "SESSION_NOT_FOUND", *error);
                }
                package_req.session_id = selected_session_id;
                package_req.session_jsonl_path = session_jsonl;
                package_req.workspace_hash = meta.no_workspace ? std::string{} : ws.hash;
                selected_workspace_hash = package_req.workspace_hash;
            }

            auto package = acecode::feedback::build_feedback_package(package_req);
            if (!package.ok) {
                return json_err(req, 500, "PACKAGE_FAILED", package.error);
            }

            acecode::feedback::FeedbackUploadRequest upload_req;
            upload_req.upload_url = upload_url;
            upload_req.package_path = package.package_path;
            upload_req.package_filename = package.package_filename;
            upload_req.timeout_ms = upgrade_cfg.timeout_ms;

            auto upload = acecode::feedback::upload_feedback_package(upload_req);
            if (!upload.ok) {
                return json_err(req, 502, "UPLOAD_FAILED", upload.error,
                                json{{"upload_url", upload_url},
                                     {"package_path", path_to_utf8(package.package_path)},
                                     {"package_filename", package.package_filename}});
            }

            std::error_code ec;
            fs::remove(package.package_path, ec);

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{
                {"ok", true},
                {"package_filename", package.package_filename},
                {"log_included", package.log_included},
                {"log_tail_bytes", static_cast<std::uint64_t>(package.log_tail_bytes)},
                {"included_files", package.included_files},
                {"selected_session_id", selected_session_id.empty()
                    ? json(nullptr)
                    : json(selected_session_id)},
                {"workspace_hash", selected_workspace_hash},
            }.dump();
            return with_cors(req, std::move(r));
        });
    }

void WebServer::Impl::register_mcp() {
        // GET /api/mcp: 读 config 当前 mcp_servers 段。spec 9.8
        CROW_ROUTE(app, "/api/mcp").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json out = json::object();
            if (deps.app_config) {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                for (const auto& [name, srv] : deps.app_config->mcp_servers) {
                    json o;
                    switch (srv.transport) {
                        case McpTransport::Stdio: o["transport"] = "stdio"; break;
                        case McpTransport::Sse:   o["transport"] = "sse";   break;
                        case McpTransport::Http:  o["transport"] = "http";  break;
                    }
                    if (!srv.command.empty()) o["command"] = srv.command;
                    if (!srv.args.empty())    o["args"]    = srv.args;
                    if (!srv.env.empty())     o["env"]     = srv.env;
                    if (!srv.url.empty())     o["url"]     = srv.url;
                    if (!srv.sse_endpoint.empty()) o["sse_endpoint"] = srv.sse_endpoint;
                    if (!srv.headers.empty()) o["headers"] = srv.headers;
                    // auth_token 不回写,避免日志 / 浏览器缓存泄漏
                    o["timeout_seconds"]      = srv.timeout_seconds;
                    // 仅禁用时透出,启用态保持字段稀疏(前端 enabled = !disabled)。
                    if (srv.disabled) o["disabled"] = true;
                    out[name] = std::move(o);
                }
            }
            crow::response r(out.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // PUT /api/mcp: 覆盖写 mcp_servers 段(不自动 reload)。spec 9.8
        CROW_ROUTE(app, "/api/mcp").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            try {
                auto j = json::parse(req.body);
                if (!j.is_object()) {
                    crow::response r(400);
                    r.body = R"({"error":"body must be a JSON object"})";
                    r.add_header("Content-Type", "application/json");
                    return r;
                }
                std::map<std::string, McpServerConfig> new_servers;
                for (auto it = j.begin(); it != j.end(); ++it) {
                    McpServerConfig cfg;
                    const auto& v = it.value();
                    auto t = v.value("transport", std::string("stdio"));
                    if (t == "sse")  cfg.transport = McpTransport::Sse;
                    else if (t == "http") cfg.transport = McpTransport::Http;
                    else cfg.transport = McpTransport::Stdio;
                    cfg.command      = v.value("command", std::string{});
                    if (v.contains("args") && v["args"].is_array())
                        cfg.args = v["args"].get<std::vector<std::string>>();
                    if (v.contains("env") && v["env"].is_object())
                        cfg.env  = v["env"].get<std::map<std::string,std::string>>();
                    cfg.url          = v.value("url", std::string{});
                    cfg.sse_endpoint = v.value("sse_endpoint", std::string("/sse"));
                    if (v.contains("headers") && v["headers"].is_object())
                        cfg.headers = v["headers"].get<std::map<std::string,std::string>>();
                    cfg.auth_token   = v.value("auth_token", std::string{});
                    cfg.timeout_seconds = v.value("timeout_seconds", 30);
                    cfg.disabled     = v.value("disabled", false);
                    new_servers.emplace(it.key(), std::move(cfg));
                }
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                deps.app_config->mcp_servers = std::move(new_servers);
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
                crow::response r(200);
                r.body = R"({"saved":true,"reload_required":true})";
                r.add_header("Content-Type", "application/json");
                return r;
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return r;
            }
        });

        // POST /api/mcp/reload: spec 9.9。
        // v1 简化实现: 不真正触发 cpp-mcp 重连(那需要把 mcp client 拉到
        // WebServerDeps 里,改动面更大)。返回 not_implemented + 提示用户
        // 重启 daemon,后续 change 再补。
        CROW_ROUTE(app, "/api/mcp/reload").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            crow::response r(501);
            r.body = R"({"error":"mcp reload not implemented in v1; restart daemon to pick up changes"})";
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // POST /api/mcp/toggle body {name, enabled}: 单个 server 的启用开关。
        // 落盘 config.disabled(重启后仍生效)+ 运行时经 McpManager 免重启热切换
        // (整个 app 的所有会话共享同一 ToolExecutor,disable 会立刻注销其工具)。
        CROW_ROUTE(app, "/api/mcp/toggle").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto reply = [](int code, const json& body) {
                crow::response r(code);
                r.body = body.dump();
                r.add_header("Content-Type", "application/json");
                return r;
            };

            try {
                auto j = json::parse(req.body);
                if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
                    return reply(400, json{{"error", "expected {name, enabled}"}});
                }
                const std::string name = j["name"].get<std::string>();
                const bool enabled = j.value("enabled", true);

                bool found = false;
                {
                    std::lock_guard<std::mutex> config_lock(app_config_mu);
                    auto it = deps.app_config->mcp_servers.find(name);
                    if (it != deps.app_config->mcp_servers.end()) {
                        found = true;
                        it->second.disabled = !enabled;
                        if (!deps.config_path.empty()) {
                            save_config(*deps.app_config, deps.config_path);
                        } else {
                            save_config(*deps.app_config);
                        }
                    }
                }
                if (!found) {
                    return reply(404, json{{"error", "unknown mcp server"}});
                }

                // 运行时热切换:锁外调用,避免持 app_config_mu 期间进 manager 锁。
                // manager 缺失(测试 fixture)或未登记该 server 时降级为仅落盘,
                // 前端据 applied=false 提示需重启。
                bool applied = false;
                if (deps.mcp_manager && deps.tools && deps.mcp_manager->has_server(name)) {
                    applied = enabled ? deps.mcp_manager->enable(name, *deps.tools)
                                      : deps.mcp_manager->disable(name, *deps.tools);
                }
                return reply(200, json{{"name", name}, {"enabled", enabled}, {"applied", applied}});
            } catch (const std::exception& e) {
                return reply(400, json{{"error", std::string("bad json: ") + e.what()}});
            }
        });
    }

void WebServer::Impl::register_health() {
        // GET /api/health: spec 9.2
        // 不强制 token (loopback / 远程都返回,为了让前端在加载时探活)。
        // 但不暴露敏感信息(没有 token / cwd 之外的本机路径等)。
        CROW_ROUTE(app, "/api/health").methods(crow::HTTPMethod::GET)
        ([this](const crow::request&) {
            json j;
            j["guid"]            = deps.guid;
            j["pid"]             = deps.pid;
            j["port"]            = deps.web_cfg ? deps.web_cfg->port : 0;
            j["version"]         = ACECODE_VERSION;
            j["cwd"]             = deps.cwd;
            j["uptime_seconds"]  = (now_unix_ms() - deps.start_time_unix_ms) / 1000;
            // desktop.notifications 透传给前端,desktopNotify.js 用它判抑制规则
            // (见 openspec/changes/add-desktop-attention-notifications)。
            // 浏览器直连 daemon 模式没有桌面壳桥,前端会自然 no-op,这里始终输出。
            if (deps.app_config) {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                const auto& n = deps.app_config->desktop.notifications;
                j["notifications"] = {
                    {"enabled", n.enabled},
                    {"on_question", n.on_question},
                    {"on_completion", n.on_completion},
                    {"suppress_when_focused", n.suppress_when_focused},
                };
                j["features"] = {
                    {"completed_turn_self_heal", {
                        {"enabled", deps.app_config->features.completed_turn_self_heal},
                    }},
                };
            }
            // 控制台 capability(add-console-dock):前端据此显示/隐藏入口,
            // backend=="pipe" 时显示 legacy 降级提示。
            j["console"] = {
                {"available", deps.pty_registry != nullptr},
                {"backend", deps.pty_registry
                    ? pty_backend_kind_name(deps.pty_registry->backend()) : ""},
            };
            crow::response r(j.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // GET /api/model-pool-status: PUB 模型池负载快照(load-monitor 缓存)。
        // 非敏感数据,与 /api/health 一样不强制 token,便于前端轮询展示负载。
        // 服务未启动 / 尚无数据时 models 为空数组,前端自然不显示负载指示。
        CROW_ROUTE(app, "/api/model-pool-status").methods(crow::HTTPMethod::GET)
        ([](const crow::request&) {
            auto snap = acecode::model_pool_status_service().snapshot();
            json models = json::array();
            for (const auto& kv : snap) {
                json o;
                o["modelPoolName"]          = kv.first;
                o["usageRate"]              = kv.second.usage_rate;
                o["maxWindowTokens"]        = kv.second.max_window_tokens;
                o["effectiveContextWindow"] =
                    acecode::effective_context_window(kv.second.max_window_tokens);
                models.push_back(std::move(o));
            }
            json j;
            j["models"] = std::move(models);
            crow::response r(j.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });
    }

void WebServer::Impl::register_usage() {
        CROW_ROUTE(app, "/api/usage").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        CROW_ROUTE(app, "/api/usage").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            auto query = usage_query_from_request(req);
            auto scopes = usage_scopes_for_request(query.workspace_hash);
            auto aggregate = aggregate_usage_ledgers(scopes, query);
            auto body = usage_aggregate_to_json(aggregate);
            if (!query.workspace_hash.empty()) {
                body["metadata"]["workspace_filter"] = query.workspace_hash;
            }
            crow::response r(body.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
    }

void WebServer::Impl::register_history() {
        CROW_ROUTE(app, "/api/history").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // GET /api/history?cwd=<cwd>&max=N: 拉 per-cwd 历史(与 TUI 共享同一份文件)
        CROW_ROUTE(app, "/api/history").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            std::string cwd;
            if (auto c = req.url_params.get("cwd")) cwd = c;
            if (cwd.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd parameter required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            int max = 0;
            if (auto m = req.url_params.get("max")) {
                try { max = std::stoi(m); } catch (...) { max = 0; }
            }
            InputHistoryConfig input_history;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                input_history = deps.app_config->input_history;
            }
            auto arr = load_history(cwd, max, input_history);
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/history body {text}: 追加。enabled=false 静默丢弃。
        // 用 daemon 自己的 cwd(deps.cwd)。
        CROW_ROUTE(app, "/api/history").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            std::string text;
            try {
                auto j = json::parse(req.body);
                text = j.value("text", std::string{});
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            InputHistoryConfig input_history;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                input_history = deps.app_config->input_history;
            }
            append_history(deps.cwd, text, input_history);
            return with_cors(req, crow::response(204));
        });
    }

void WebServer::Impl::register_static() {
        // 静态资源直接按文件实际路径服务,不加 /static/ 前缀:
        //   /app.js                                  → web/app.js
        //   /components/ace-chat.js                  → web/components/ace-chat.js
        //   /vendor/bootstrap/fonts/x.woff2          → web/vendor/bootstrap/fonts/x.woff2
        // lookup miss 的请求 fallback 到 index.html 供 SPA 路由(/sessions/<id> 等)。
        // /api/*、/ws/* 由前面 explicit route 处理;若误进来直接 404。
        //
        // Crow 1.3.2 quirk: <path> 模板会破坏 server bind,catchall 对深路径也不
        // fallthrough。<string> 单段匹配可靠,所以按层数列举(前端最深 4 段:
        // vendor/bootstrap/fonts/x.woff2),catchall 兜底处理 / 与未匹配 path。
        // 不走 require_auth(index.html / 资源必须无 token 加载,否则
        // 前端 token-prompt 自身都进不去)。
        auto serve_path = [this](const crow::request& req, const std::string& full_url) -> crow::response {
            // /api/*、/ws/* 没匹配到 explicit route 时直接 404,不 fallback 到 SPA。
            if (full_url.rfind("/api/", 0) == 0 || full_url == "/api"
             || full_url.rfind("/ws/",  0) == 0 || full_url == "/ws") {
                return crow::response(404);
            }
            if (!assets) return crow::response(503);

            std::string path = (!full_url.empty() && full_url[0] == '/') ? full_url.substr(1) : full_url;
            auto qpos = path.find('?');
            if (qpos != std::string::npos) path.resize(qpos);

            if (!path.empty()) {
                auto r = assets->lookup(path);
                if (r.has_value()) {
                    crow::response resp(200);
                    resp.body.assign(reinterpret_cast<const char*>(r->data), r->size);
                    resp.add_header("Content-Type", r->content_type);
                    if (req.url_params.get("v")) {
                        resp.add_header("Cache-Control", "public, max-age=31536000, immutable");
                    } else {
                        resp.add_header("Cache-Control", "no-cache");
                    }
                    return resp;
                }
            }

            // SPA fallback: 任何不存在的资源都返回 index.html(让前端 hash 路由处理)
            auto idx = assets->lookup("index.html");
            if (!idx.has_value()) {
                crow::response resp(503);
                resp.body = "index.html missing — front-end not bundled";
                return resp;
            }
            crow::response resp(200);
            resp.body.assign(reinterpret_cast<const char*>(idx->data), idx->size);
            resp.add_header("Content-Type", idx->content_type);
            resp.add_header("Cache-Control", "no-cache");
            return resp;
        };

        // 显式 / 路由 — 不能让 / 走 CATCHALL,因为 Crow 1.3.2 master 在 catchall
        // 处理后会污染同 keep-alive 连接的下个请求(残留 connection 小写 header,
        // 下一个响应回 Content-Length:0、丢 Content-Type)。复现:同 socket
        // GET / 然后 GET /api/health → 第二个回空。绕过办法是把 / 拦在 catchall
        // 之前,走 explicit handler,catchall 仅用作未知深路径兜底。
        CROW_ROUTE(app, "/")
        ([serve_path](const crow::request& req) {
            return serve_path(req, "/");
        });
        CROW_ROUTE(app, "/<string>")
        ([serve_path](const crow::request& req, std::string a) {
            return serve_path(req, "/" + a);
        });
        CROW_ROUTE(app, "/<string>/<string>")
        ([serve_path](const crow::request& req, std::string a, std::string b) {
            return serve_path(req, "/" + a + "/" + b);
        });
        CROW_ROUTE(app, "/<string>/<string>/<string>")
        ([serve_path](const crow::request& req, std::string a, std::string b, std::string c) {
            return serve_path(req, "/" + a + "/" + b + "/" + c);
        });
        CROW_ROUTE(app, "/<string>/<string>/<string>/<string>")
        ([serve_path](const crow::request& req, std::string a, std::string b, std::string c, std::string d) {
            return serve_path(req, "/" + a + "/" + b + "/" + c + "/" + d);
        });

        // 未列举层数的兜底(>4 段)。/ 由上面 explicit 路由处理。
        CROW_CATCHALL_ROUTE(app)
        ([serve_path](const crow::request& req) {
            return serve_path(req, std::string(req.url));
        });
    }

void WebServer::Impl::register_ui_preferences() {
        CROW_ROUTE(app, "/api/config/ui-preferences").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/config/custom-instructions").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/config/connectors").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/config/upgrade").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/config/default-permission-mode").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/update/status").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/update/start").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/config/ace-browser-bridge").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // GET /api/config/ui-preferences: non-sensitive Web/Desktop UI prefs.
        CROW_ROUTE(app, "/api/config/ui-preferences").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = ui_preferences_to_json(deps.app_config->web_ui).dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/config/custom-instructions: user-authored prompt context.
        CROW_ROUTE(app, "/api/config/custom-instructions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = custom_instructions_to_json(
                deps.app_config->custom_instructions).dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/config/connectors: configured desktop connectors.
        CROW_ROUTE(app, "/api/config/connectors").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"connectors", connectors_to_json(
                deps.app_config->connectors)}}.dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/config/default-permission-mode: daemon default for new sessions.
        CROW_ROUTE(app, "/api/config/default-permission-mode").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            refresh_default_session_preferences_for_new_session_locked();
            auto parsed = parse_permission_mode_name(deps.app_config->default_permission_mode);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = permission_mode_to_json(parsed.value_or(PermissionMode::Default)).dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/config/upgrade: self-upgrade service settings.
        CROW_ROUTE(app, "/api/config/upgrade").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = upgrade_config_to_json(deps.app_config->upgrade).dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/update/status: manifest-only update availability check.
        CROW_ROUTE(app, "/api/update/status").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            auto result = acecode::upgrade::check_for_update(*deps.app_config,
                                                             ACECODE_VERSION);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = update_check_to_json(result).dump();
            return with_cors(req, std::move(r));
        });

        // POST /api/update/start: explicit user-triggered acecode update.
        CROW_ROUTE(app, "/api/update/start").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            acecode::upgrade::UpdateCheckResult result;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                result = acecode::upgrade::check_for_update(*deps.app_config,
                                                            ACECODE_VERSION);
            }
            if (!result.update_available()) {
                crow::response r(409);
                r.add_header("Content-Type", "application/json");
                r.body = json{{"error", "NO_UPDATE"},
                              {"message", "no compatible update is available"},
                              {"status", update_check_to_json(result)}}.dump();
                return with_cors(req, std::move(r));
            }

            std::string start_error;
            bool started = deps.start_update_command
                ? deps.start_update_command(&start_error)
                : start_default_update_command(&start_error);
            if (!started) {
                return json_err(500, "START_FAILED", start_error);
            }

            crow::response r(202);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"started", true},
                          {"latest_version", result.latest_version},
                          {"message", "acecode update started"}}.dump();
            return with_cors(req, std::move(r));
        });

        // PUT /api/config/custom-instructions body {text:string}.
        CROW_ROUTE(app, "/api/config/custom-instructions").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, "BAD_JSON", std::string("invalid JSON body: ") + e.what());
            }
            if (!body.is_object() ||
                !body.contains("text") ||
                !body["text"].is_string()) {
                return json_err(400, "BAD_REQUEST", "expected {text: string}");
            }

            const std::string text = body["text"].get<std::string>();
            if (text.size() > kCustomInstructionsMaxBytes) {
                return json_err(400, "BAD_REQUEST",
                                "custom instructions exceed " +
                                std::to_string(kCustomInstructionsMaxBytes) + " bytes");
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            const auto before = deps.app_config->custom_instructions;
            deps.app_config->custom_instructions.set_text(text);
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->custom_instructions = before;
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = custom_instructions_to_json(
                deps.app_config->custom_instructions).dump();
            return with_cors(req, std::move(r));
        });

        // PUT /api/config/connectors body {connectors:[...]}.
        CROW_ROUTE(app, "/api/config/connectors").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, "BAD_JSON", std::string("invalid JSON body: ") + e.what());
            }
            if (!body.is_object() || !body.contains("connectors")) {
                return json_err(400, "BAD_REQUEST", "expected {connectors: array}");
            }

            std::vector<ConnectorConfig> parsed;
            std::string parse_error;
            if (!parse_connectors_json(body["connectors"], parsed, &parse_error)) {
                return json_err(400, "BAD_REQUEST", parse_error);
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            const auto before = deps.app_config->connectors;
            deps.app_config->connectors = std::move(parsed);
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->connectors = before;
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"connectors", connectors_to_json(
                deps.app_config->connectors)}}.dump();
            return with_cors(req, std::move(r));
        });

        // PUT /api/config/default-permission-mode body {mode:string}.
        CROW_ROUTE(app, "/api/config/default-permission-mode").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, "BAD_JSON", std::string("invalid JSON body: ") + e.what());
            }
            if (!body.is_object() ||
                !body.contains("mode") ||
                !body["mode"].is_string()) {
                return json_err(400, "BAD_REQUEST", "expected {mode: string}");
            }
            auto mode = parse_permission_mode_name(body["mode"].get<std::string>());
            if (!mode.has_value()) {
                return json_err(400, "INVALID_PERMISSION_MODE", "invalid permission mode");
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            const std::string before = deps.app_config->default_permission_mode;
            deps.app_config->default_permission_mode = PermissionManager::mode_name(*mode);
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->default_permission_mode = before;
                return json_err(500, "PERSIST_FAILED", e.what());
            }
            if (deps.session_registry) {
                deps.session_registry->set_default_permission_mode(*mode);
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = permission_mode_to_json(*mode).dump();
            return with_cors(req, std::move(r));
        });

        // PUT /api/config/upgrade body {base_url:string}.
        CROW_ROUTE(app, "/api/config/upgrade").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, "BAD_JSON", std::string("invalid JSON body: ") + e.what());
            }
            if (!body.is_object() ||
                !body.contains("base_url") ||
                !body["base_url"].is_string()) {
                return json_err(400, "BAD_REQUEST", "expected {base_url: string}");
            }

            const std::string normalized =
                normalize_upgrade_base_url(body["base_url"].get<std::string>());
            if (!is_valid_upgrade_base_url(normalized)) {
                return json_err(400, "BAD_REQUEST",
                                "upgrade.base_url must be a non-empty http or https URL");
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            const auto before = deps.app_config->upgrade;
            deps.app_config->upgrade.base_url = normalized;
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->upgrade = before;
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = upgrade_config_to_json(deps.app_config->upgrade).dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/config/ace-browser-bridge: browser bridge tool settings.
        CROW_ROUTE(app, "/api/config/ace-browser-bridge").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = ace_browser_bridge_settings_to_json(
                deps.app_config->ace_browser_bridge).dump();
            return with_cors(req, std::move(r));
        });

        // PUT /api/config/ace-browser-bridge body {enabled:boolean}.
        CROW_ROUTE(app, "/api/config/ace-browser-bridge").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, "BAD_JSON", std::string("invalid JSON body: ") + e.what());
            }
            if (!body.is_object() ||
                !body.contains("enabled") ||
                !body["enabled"].is_boolean()) {
                return json_err(400, "BAD_REQUEST", "expected {enabled: boolean}");
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            const auto before = deps.app_config->ace_browser_bridge;
            deps.app_config->ace_browser_bridge.enabled = body["enabled"].get<bool>();
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->ace_browser_bridge = before;
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            if (deps.tools) {
                ace_browser_bridge::unregister_ace_browser_bridge_tools(*deps.tools);
                if (deps.app_config->ace_browser_bridge.enabled) {
                    ace_browser_bridge::register_ace_browser_bridge_tools(
                        *deps.tools, deps.app_config->ace_browser_bridge);
                }
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = ace_browser_bridge_settings_to_json(
                deps.app_config->ace_browser_bridge).dump();
            return with_cors(req, std::move(r));
        });

        // POST /api/models/probe: best-effort OpenAI-compatible /models probe.
        CROW_ROUTE(app, "/api/models/probe").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, "BAD_JSON", std::string("invalid JSON body: ") + e.what());
            }

            std::string err_code;
            std::string err;
            auto parsed = parse_model_probe_request(body, err_code, err);
            if (!parsed) {
                return json_err(400, err_code.empty() ? "BAD_REQUEST" : err_code.c_str(), err);
            }

            if (parsed->provider == "copilot") {
                const std::string github_token = load_github_token();
                if (github_token.empty()) {
                    return json_err(401,
                                    "COPILOT_AUTH_REQUIRED",
                                    "GitHub Copilot authentication is required");
                }

                CopilotModelsResult result = fetch_copilot_model_ids(github_token);
                if (!result.error.empty()) {
                    const int status = result.status_code == 401 ? 401 : 502;
                    return json_err(status,
                                    result.error.c_str(),
                                    result.message.empty()
                                        ? "Copilot model discovery failed"
                                        : result.message);
                }

                crow::response r(json{{"models", result.models}}.dump());
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const std::string url = trim_trailing_slash(parsed->base_url) + "/models";
            cpr::Header headers = {{"Content-Type", "application/json"}};
            if (!parsed->api_key.empty()) {
                headers["Authorization"] = "Bearer " + parsed->api_key;
            }
            std::string header_error;
            auto resolved_headers = resolve_request_headers(parsed->request_headers, header_error);
            if (!resolved_headers.has_value()) {
                return json_err(400, "INVALID_REQUEST_HEADER", header_error);
            }
            for (const auto& [k, v] : *resolved_headers) {
                headers[k] = v;
            }
            auto proxy_opts = network::proxy_options_for(url);
            cpr::Response response = cpr::Get(
                cpr::Url{url},
                headers,
                network::build_ssl_options(proxy_opts),
                proxy_opts.proxies,
                proxy_opts.auth,
                cpr::Timeout{10000}
            );

            if (response.status_code == 0) {
                return json_err(502, "PROBE_FAILED", response.error.message);
            }
            if (response.status_code < 200 || response.status_code >= 300) {
                return json_err(502, "PROBE_HTTP_ERROR",
                                "upstream returned HTTP " + std::to_string(response.status_code));
            }

            try {
                auto parsed = parse_openai_models(json::parse(response.text));
                json out;
                out["models"] = parsed.ids;
                if (!parsed.context_windows.empty()) {
                    out["model_context_windows"] = parsed.context_windows;
                }
                crow::response r(out.dump());
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            } catch (const std::exception& e) {
                return json_err(502, "PROBE_BAD_JSON", e.what());
            }
        });

        // PUT /api/config/ui-preferences body {show_acecode_avatar:boolean}.
        // Kept for older web clients; ACECode avatar display is now permanently
        // disabled and the persisted value is normalized to false.
        CROW_ROUTE(app, "/api/config/ui-preferences").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            json body;
            try { body = json::parse(req.body); }
            catch (const std::exception& e) {
                return json_err(400, "BAD_JSON", std::string("invalid JSON body: ") + e.what());
            }
            if (!body.is_object() ||
                !body.contains("show_acecode_avatar") ||
                !body["show_acecode_avatar"].is_boolean()) {
                return json_err(400, "BAD_REQUEST",
                                "expected {show_acecode_avatar: boolean}");
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            const auto before = deps.app_config->web_ui;
            deps.app_config->web_ui.show_acecode_avatar = false;
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->web_ui = before;
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = ui_preferences_to_json(deps.app_config->web_ui).dump();
            return with_cors(req, std::move(r));
        });
    }
} // namespace acecode::web
