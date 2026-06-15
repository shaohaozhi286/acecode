// routes_models.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

void WebServer::Impl::register_models() {
        CROW_ROUTE(app, "/api/models").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/models/<string>").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/models/probe").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/config/default-model").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/copilot/auth").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/copilot/auth/device").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/copilot/auth/device/poll").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/model").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });

        // GET /api/models: 返回 saved_models
        CROW_ROUTE(app, "/api/models").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            auto arr = list_models(*deps.app_config);
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/config/default-model: 返回当前 default_model_name
        // (给 WebUI 模型设置页标星用)。读 cfg 的字段,空字符串也照返。
        CROW_ROUTE(app, "/api/config/default-model").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            refresh_default_session_preferences_for_new_session();
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"name", deps.app_config->default_model_name}}.dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/copilot/auth: local credential status only; never returns tokens.
        CROW_ROUTE(app, "/api/copilot/auth").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            const bool has_token = has_saved_github_token();
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{
                {"provider", "copilot"},
                {"has_token", has_token},
                {"authenticated", has_token}
            }.dump();
            return with_cors(req, std::move(r));
        });

        // DELETE /api/copilot/auth: remove saved GitHub token; saved_models stay intact.
        CROW_ROUTE(app, "/api/copilot/auth").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            std::string error;
            if (!delete_github_token(&error)) {
                crow::response r(500);
                r.add_header("Content-Type", "application/json");
                r.body = json{{"error", "DELETE_FAILED"}, {"message", error}}.dump();
                return with_cors(req, std::move(r));
            }
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{
                {"ok", true},
                {"provider", "copilot"},
                {"has_token", false},
                {"authenticated", false}
            }.dump();
            return with_cors(req, std::move(r));
        });

        // POST /api/copilot/auth/device: start GitHub OAuth device flow.
        CROW_ROUTE(app, "/api/copilot/auth/device").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            DeviceCodeResponse dc = request_device_code();
            if (dc.device_code.empty()) {
                crow::response r(502);
                r.add_header("Content-Type", "application/json");
                r.body = json{
                    {"error", "DEVICE_CODE_FAILED"},
                    {"message", "failed to request GitHub device code"}
                }.dump();
                return with_cors(req, std::move(r));
            }
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{
                {"status", "pending"},
                {"provider", "copilot"},
                {"device_code", dc.device_code},
                {"user_code", dc.user_code},
                {"verification_uri", dc.verification_uri},
                {"interval", dc.interval},
                {"expires_in", dc.expires_in},
                {"expires_at_unix_ms", now_unix_ms() + static_cast<std::int64_t>(dc.expires_in) * 1000}
            }.dump();
            return with_cors(req, std::move(r));
        });

        // POST /api/copilot/auth/device/poll body {device_code}: one poll tick.
        CROW_ROUTE(app, "/api/copilot/auth/device/poll").methods(crow::HTTPMethod::POST)
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
            if (!body.is_object() ||
                !body.contains("device_code") ||
                !body["device_code"].is_string() ||
                body["device_code"].get<std::string>().empty()) {
                return json_err(400, "BAD_REQUEST", "expected {device_code: string}");
            }

            DevicePollResult poll = poll_for_access_token_once(
                body["device_code"].get<std::string>());
            if (poll.status == "authorized") {
                CopilotToken ct = exchange_copilot_token(poll.access_token);
                if (ct.token.empty()) {
                    return json_err(401,
                                    "COPILOT_TOKEN_EXCHANGE_FAILED",
                                    "GitHub login succeeded, but Copilot token exchange failed");
                }
                save_github_token(poll.access_token);
                crow::response r(200);
                r.add_header("Content-Type", "application/json");
                r.body = json{
                    {"status", "authenticated"},
                    {"provider", "copilot"},
                    {"authenticated", true},
                    {"has_token", true}
                }.dump();
                return with_cors(req, std::move(r));
            }

            crow::response r(poll.status == "failed" ? 400 : 200);
            r.add_header("Content-Type", "application/json");
            r.body = json{
                {"status", poll.status.empty() ? "failed" : poll.status},
                {"provider", "copilot"},
                {"authenticated", false},
                {"has_token", has_saved_github_token()},
                {"error", poll.error},
                {"message", poll.message},
                {"interval_delta_seconds", poll.interval_delta_seconds}
            }.dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/sessions/:id/model: 返回当前 session model state。
        CROW_ROUTE(app, "/api/sessions/<string>/model").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& sid) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_registry) return crow::response(503);

            std::string workspace_hash;
            if (auto w = req.url_params.get("workspace")) workspace_hash = w;

            auto state = current_model_state_for_session(sid, workspace_hash);
            if (!state.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"session not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(model_state_to_json(*state).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/model body {name}: 切当前 effective model
        CROW_ROUTE(app, "/api/sessions/<string>/model").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& sid) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            if (!deps.session_registry) return crow::response(503);

            // 校验 session 存在
            if (!deps.session_registry->lookup(sid)) {
                crow::response r(404);
                r.body = R"({"error":"session not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            std::string name;
            try {
                auto j = json::parse(req.body);
                name = j.value("name", std::string{});
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (name.empty()) {
                crow::response r(400);
                r.body = R"({"error":"name required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto entry = find_model_by_name(*deps.app_config, name);
            if (!entry.has_value()) {
                crow::response r(400);
                r.body = json{{"error", "Unknown model name: " + name}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            SessionModelState state;
            std::string error;
            if (!deps.session_registry->switch_model(sid, *entry, &state, &error)) {
                crow::response r(error == "session not found" ? 404 : 500);
                r.body = json{{"error", error.empty() ? "model switch failed" : error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(model_state_to_json(state).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/models: 新增 saved_models 条目。body = SavedModelDraft JSON。
        // 失败时 cfg 不变;落盘失败时 cfg 内存回滚保持与磁盘一致。
        CROW_ROUTE(app, "/api/models").methods(crow::HTTPMethod::POST)
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
            std::string err;
            auto draft = parse_model_draft(body, err);
            if (!draft) return json_err(400, "BAD_REQUEST", err);

            auto rc = add_saved_model(*deps.app_config, *draft);
            if (rc != SavedModelEditError::OK) {
                return json_err(http_status_for_edit_error(rc),
                                to_string(rc),
                                "saved_models add rejected");
            }
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->saved_models.pop_back();  // 回滚
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = profile_to_safe_json(deps.app_config->saved_models.back()).dump();
            return with_cors(req, std::move(r));
        });

        // PUT /api/models/<name>: 更新 saved_models 条目(可改名)。
        CROW_ROUTE(app, "/api/models/<string>").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& url_name) {
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
            std::string err;
            auto draft = parse_model_draft(body, err);
            if (!draft) return json_err(400, "BAD_REQUEST", err);

            // patch 语义:body 没显式带 api_key / base_url 时,从 existing 条目
            // 注入旧值再走校验。这样旧客户端编辑表单不必每次让用户重输
            // api_key。也覆盖 base_url 以防偶发未提交;model/provider/name 显式必填,
            // 不参与 patch。
            if (body.is_object()) {
                const ModelProfile* existing = nullptr;
                for (const auto& e : deps.app_config->saved_models) {
                    if (e.name == url_name) { existing = &e; break; }
                }
                if (existing) {
                    if (!body.contains("api_key")) draft->api_key = existing->api_key;
                    if (!body.contains("base_url")) draft->base_url = existing->base_url;
                    if (!body.contains("context_window")) {
                        draft->context_window = existing->context_window;
                    }
                    if (!body.contains("stream_timeout_ms")) {
                        draft->stream_timeout_ms = existing->stream_timeout_ms;
                    }
                    if (!body.contains("capabilities")) {
                        draft->capabilities = existing->capabilities;
                    }
                    if (!body.contains("request_headers")) {
                        draft->request_headers = existing->request_headers;
                    }
                }
            }

            auto snapshot = deps.app_config->saved_models;
            auto rc = update_saved_model(*deps.app_config, url_name, *draft);
            if (rc != SavedModelEditError::OK) {
                return json_err(http_status_for_edit_error(rc),
                                to_string(rc),
                                "saved_models update rejected");
            }
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->saved_models = std::move(snapshot);
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            // 找到刚改完的条目(name 可能与 url_name 不同)
            const ModelProfile* updated = nullptr;
            for (const auto& e : deps.app_config->saved_models) {
                if (e.name == draft->name) { updated = &e; break; }
            }
            if (!updated) {
                // 理论不可达:update_saved_model 返回 OK 意味着条目已存在
                // 且 name == draft->name。真走到这里说明并发突变或内部状态
                // 异常 — 别静默吐空 body,显式 500 让前端能看到。
                return json_err(500, "INVARIANT_BROKEN",
                                "post-update entry not found in saved_models");
            }
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = profile_to_safe_json(*updated).dump();
            return with_cors(req, std::move(r));
        });

        // DELETE /api/models/<name>: 删除 saved_models 条目。多模型时 default 不能删;
        // 唯一 default 可删并清空 default_model_name。
        CROW_ROUTE(app, "/api/models/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& url_name) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);

            auto json_err = [&](int status, const char* code, const std::string& msg) {
                crow::response r(status);
                r.body = json{{"error", code}, {"message", msg}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            auto snapshot = deps.app_config->saved_models;
            auto default_snapshot = deps.app_config->default_model_name;
            auto rc = remove_saved_model(*deps.app_config, url_name);
            if (rc != SavedModelEditError::OK) {
                return json_err(http_status_for_edit_error(rc),
                                to_string(rc),
                                "saved_models remove rejected");
            }
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->saved_models = std::move(snapshot);
                deps.app_config->default_model_name = std::move(default_snapshot);
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"ok", true}}.dump();
            return with_cors(req, std::move(r));
        });

        // POST /api/config/default-model body {name}: 设置 cfg.default_model_name。
        // name 必须存在于 saved_models。
        CROW_ROUTE(app, "/api/config/default-model").methods(crow::HTTPMethod::POST)
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
            if (!body.is_object() || !body.contains("name") || !body["name"].is_string()) {
                return json_err(400, "BAD_REQUEST", "expected {name: string}");
            }
            std::string name = body["name"].get<std::string>();

            bool found = false;
            for (const auto& e : deps.app_config->saved_models) {
                if (e.name == name) { found = true; break; }
            }
            if (!found) return json_err(404, "NOT_FOUND", "no such model name");

            std::string before = deps.app_config->default_model_name;
            deps.app_config->default_model_name = name;
            try {
                if (!deps.config_path.empty()) {
                    save_config(*deps.app_config, deps.config_path);
                } else {
                    save_config(*deps.app_config);
                }
            } catch (const std::exception& e) {
                deps.app_config->default_model_name = before;
                return json_err(500, "PERSIST_FAILED", e.what());
            }

            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = json{{"default_model_name", name}}.dump();
            return with_cors(req, std::move(r));
        });
    }
} // namespace acecode::web
