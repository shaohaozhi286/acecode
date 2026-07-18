// routes_sessions.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"
#include "../../session/compact_checkpoint.hpp"
#include "../../session/session_user_message_search.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace acecode::web {

using nlohmann::json;

namespace {

// Crow 的 url_params.get() 返回值已经过一次 qs_decode('+'→空格、%xx 解码),
// 这里绝不能再 decode 一遍:查询里的字面 '+' / '%'(如 "C++")会被二次解码破坏。
std::string trim_query_param(const char* raw) {
    if (!raw) return {};
    std::string value(raw);
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

int parse_search_limit(const char* raw) {
    if (!raw || !*raw) return 30;
    try {
        int value = std::stoi(raw);
        if (value < 1) return 1;
        if (value > 100) return 100;
        return value;
    } catch (...) {
        return 30;
    }
}

struct UserMessageSearchScope {
    std::string project_dir;
    std::string workspace_hash;
    std::string workspace_name;
    std::string cwd;
    bool no_workspace = false;
};

} // namespace

ParsedSessionUserInputRequest
WebServer::Impl::parse_session_user_input_request(
    const std::string& body,
    const std::string& session_id,
    bool allow_worktree) {
    ParsedSessionUserInputRequest result;
    std::string text;
    std::string client_message_id;
    json attachment_refs = json::array();
    json contexts = json::array();

    try {
        auto payload = json::parse(body);
        if (!payload.is_object()) {
            result.error = "request body must be an object";
            return result;
        }
        if (payload.contains("text") && payload["text"].is_string()) {
            text = payload["text"].get<std::string>();
        }
        if (payload.contains("attachments") &&
            payload["attachments"].is_array()) {
            attachment_refs = payload["attachments"];
        }
        if (payload.contains("contexts") && payload["contexts"].is_array()) {
            contexts = payload["contexts"];
        }
        if (payload.contains("client_message_id") &&
            payload["client_message_id"].is_string()) {
            const auto candidate =
                payload["client_message_id"].get<std::string>();
            if (!candidate.empty() && candidate.size() <= 256) {
                client_message_id = candidate;
            }
        }
        if (payload.contains("expected_turn_id") &&
            payload["expected_turn_id"].is_string()) {
            result.expected_turn_id =
                payload["expected_turn_id"].get<std::string>();
        }
        if (payload.contains("worktree") &&
            payload["worktree"].is_object()) {
            const auto& worktree = payload["worktree"];
            if (worktree.contains("create") &&
                worktree["create"].is_boolean()) {
                result.worktree_create = worktree["create"].get<bool>();
            }
            if (worktree.contains("base") &&
                worktree["base"].is_string()) {
                result.worktree_base = worktree["base"].get<std::string>();
            }
        }
    } catch (const std::exception& e) {
        result.error = std::string("bad json: ") + e.what();
        return result;
    }

    if (result.worktree_create && !allow_worktree) {
        result.error = "worktree creation is not allowed for turn steering";
        return result;
    }
    if (text.empty() && attachment_refs.empty() && contexts.empty()) {
        result.error = "text or attachment required";
        return result;
    }

    // Worktree intent belongs to the ordinary first-message transaction and
    // must apply before command expansion or attachment lookup so all later
    // resolution observes the final session cwd.
    if (result.worktree_create) {
        if (!deps.session_registry) {
            result.status = 503;
            result.error = "session registry unavailable";
            return result;
        }
        auto worktree = deps.session_registry->enter_worktree_for_web(
            session_id, result.worktree_base);
        if (!worktree.ok) {
            result.status = worktree.http_status;
            result.error = worktree.error;
            return result;
        }
    }

    std::string original_text = text;
    bool expanded = false;
    SelectionPromptContext selection_context =
        build_selection_prompt_context(contexts);
    const bool selection_expanded = !selection_context.prompt.empty();

    if (attachment_refs.empty() && contexts.empty() &&
        deps.session_registry && deps.app_config) {
        if (auto entry = deps.session_registry->acquire(session_id)) {
            if (!entry->cwd.empty()) {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                auto command = web::try_expand_opencode_command(
                    text, *deps.app_config, entry->cwd);
                if (command.expanded) {
                    text = std::move(command.text);
                    expanded = true;
                }
            }
        }
    }
    if (!expanded && attachment_refs.empty() && contexts.empty() &&
        deps.session_registry && deps.app_config) {
        if (auto entry = deps.session_registry->acquire(session_id)) {
            if (!entry->cwd.empty()) {
                SkillRegistry skills;
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                initialize_skill_registry(
                    skills, *deps.app_config, entry->cwd);
                auto skill = web::try_expand_skill_command(text, skills);
                if (skill.expanded) {
                    text = std::move(skill.text);
                    expanded = true;
                }
            }
        }
    }
    if (selection_expanded) {
        text = build_selection_augmented_prompt(
            selection_context, original_text);
    }

    result.input.text = text;
    if (expanded || selection_expanded) {
        result.input.display_text = original_text;
    }
    if (!client_message_id.empty()) {
        result.input.metadata["client_message_id"] = client_message_id;
    }

    if (!attachment_refs.empty() || !contexts.empty()) {
        if (!deps.session_registry) {
            result.status = 503;
            result.error = "session registry unavailable";
            return result;
        }
        auto entry = deps.session_registry->acquire(session_id);
        if (!entry) {
            result.status = 404;
            result.error = "unknown session";
            return result;
        }

        json parts = json::array();
        if (!text.empty()) {
            parts.push_back(json{{"type", "text"}, {"text", text}});
        }

        json attachment_meta = json::array();
        const std::string project_dir =
            SessionStorage::get_project_dir(entry->cwd);
        for (const auto& ref : attachment_refs) {
            std::string attachment_id;
            if (ref.is_string()) {
                attachment_id = ref.get<std::string>();
            } else if (ref.is_object() && ref.contains("id") &&
                       ref["id"].is_string()) {
                attachment_id = ref["id"].get<std::string>();
            }
            if (attachment_id.empty()) {
                result.error = "attachment id required";
                return result;
            }

            std::string error;
            auto record =
                load_attachment(project_dir, session_id, attachment_id, &error);
            if (!record.has_value()) {
                result.status = 404;
                result.error =
                    error.empty() ? "attachment not found" : error;
                return result;
            }

            json metadata = attachment_to_json(*record);
            attachment_meta.push_back(metadata);
            const std::string part_kind = attachment_kind_for_mime(
                record->mime_type, record->name);
            parts.push_back(json{
                {"type", part_kind == "image" ? "image" : "file"},
                {"attachment", std::move(metadata)},
            });
        }

        json context_meta = json::array();
        for (const auto& context : contexts) {
            if (!context.is_object()) continue;
            if (json_string_field(context, "type") == "selection") {
                auto metadata = sanitized_selection_context_meta(context);
                if (!metadata.has_value()) continue;
                context_meta.push_back(*metadata);
                parts.push_back(json{
                    {"type", "selection_context"},
                    {"context", *metadata},
                });
            } else {
                context_meta.push_back(context);
                parts.push_back(json{
                    {"type", "browser_context"},
                    {"context", context},
                });
            }
        }

        result.input.content_parts = std::move(parts);
        if (!result.input.metadata.is_object()) {
            result.input.metadata = json::object();
        }
        if (!attachment_meta.empty()) {
            result.input.metadata["attachments"] =
                std::move(attachment_meta);
        }
        if (!context_meta.empty()) {
            result.input.metadata["contexts"] = std::move(context_meta);
        }
        if (selection_expanded) {
            result.input.metadata["selection_context_expanded"] = true;
            result.input.metadata["display_text"] = original_text;
        }
    }

    result.ok = true;
    result.status = 200;
    return result;
}

void WebServer::Impl::register_sessions() {
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/session-search/user-messages").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/messages").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/turn/steer").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/export-markdown").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/attachments").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/attachments/<string>/blob").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/commands").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/side-question").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/permissions").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/resume").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/archive").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/title").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/draft").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/todos").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/fork").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/file-checkpoints/<string>/restore").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&, const std::string&) {
            return cors_preflight(req);
        });

        // GET /api/sessions: 内存活跃 + 磁盘历史合并去重。spec 9.3
        // 默认排除 spawn_subagent 子会话;?parent=<id> 反向查询——只返回该
        // 父会话派生的后台任务子会话(「后台任务」面板数据源)。
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            const char* parent_raw = req.url_params.get("parent");
            auto arr = sessions_for_workspace(
                compatibility_workspace(),
                archived_query_requested(req),
                /*include_no_workspace=*/true,
                parent_raw ? std::string(parent_raw) : std::string{});
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/session-search/user-messages").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            const std::string query = trim_query_param(req.url_params.get("q"));
            const int limit = parse_search_limit(req.url_params.get("limit"));
            if (query.empty()) {
                crow::response r(json{{"matches", json::array()}}.dump());
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (query.size() > 512) {
                crow::response r(400);
                r.body = R"({"error":"query too long"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            std::vector<UserMessageSearchScope> scopes;
            std::unordered_set<std::string> seen_scope_keys;
            auto add_scope = [&](UserMessageSearchScope scope) {
                if (scope.project_dir.empty()) return;
                const std::string key = scope.project_dir + "|" + scope.workspace_hash +
                    "|" + (scope.no_workspace ? "no-workspace" : "workspace");
                if (!seen_scope_keys.insert(key).second) return;
                scopes.push_back(std::move(scope));
            };

            std::vector<acecode::desktop::WorkspaceMeta> workspaces;
            if (deps.workspace_registry) {
                workspaces = deps.workspace_registry->list();
            }
            if (workspaces.empty()) {
                workspaces.push_back(compatibility_workspace());
            }
            for (const auto& ws : workspaces) {
                if (ws.cwd.empty()) continue;
                add_scope(UserMessageSearchScope{
                    SessionStorage::get_project_dir(ws.cwd),
                    ws.hash,
                    ws.name.empty() ? ws.cwd : ws.name,
                    ws.cwd,
                    false,
                });
            }
            for (const auto& meta : no_workspace_disk_sessions()) {
                if (meta.cwd.empty()) continue;
                add_scope(UserMessageSearchScope{
                    SessionStorage::get_project_dir(meta.cwd),
                    std::string{},
                    std::string{},
                    meta.cwd,
                    true,
                });
            }

            json matches = json::array();
            std::unordered_set<std::string> seen_sessions;
            for (const auto& scope : scopes) {
                SessionUserMessageIndex index(scope.project_dir);
                std::string index_error;
                if (!index.ensure_project_indexed(&index_error)) {
                    LOG_WARN("[web] session user-message index refresh failed: " + index_error);
                    continue;
                }
                auto scoped_matches = index.search(query, limit, &index_error);
                if (!index_error.empty()) {
                    LOG_WARN("[web] session user-message search failed: " + index_error);
                    continue;
                }

                std::unordered_map<std::string, SessionMeta> metas;
                for (const auto& meta : SessionStorage::list_sessions(scope.project_dir)) {
                    if (meta.archived) continue;
                    if (!meta.parent_session_id.empty()) continue;
                    if (scope.no_workspace != meta.no_workspace) continue;
                    metas.emplace(meta.id, meta);
                }

                for (const auto& match : scoped_matches) {
                    auto meta_it = metas.find(match.session_id);
                    if (meta_it == metas.end()) continue;
                    const std::string session_key =
                        (scope.no_workspace ? ("no:" + scope.project_dir) : scope.workspace_hash) +
                        "|" + match.session_id;
                    if (!seen_sessions.insert(session_key).second) continue;
                    json item = session_meta_to_json(meta_it->second, scope.workspace_hash);
                    item["workspaceName"] = scope.workspace_name;
                    item["search_match"] = json{
                        {"kind", "user_message"},
                        {"score", match.score},
                        {"message_ordinal", match.message_ordinal},
                        {"snippet", match.snippet},
                        {"attachments", match.matched_attachment_names},
                    };
                    matches.push_back(std::move(item));
                }
            }

            std::sort(matches.begin(), matches.end(), [](const json& a, const json& b) {
                const int as = a.value("search_match", json::object()).value("score", 0);
                const int bs = b.value("search_match", json::object()).value("score", 0);
                if (as != bs) return as > bs;
                return a.value("updated_at", std::string{}) > b.value("updated_at", std::string{});
            });
            if (static_cast<int>(matches.size()) > limit) {
                matches.erase(matches.begin() + limit, matches.end());
            }

            crow::response r(json{{"matches", matches}}.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions: 新建 session,返回 {session_id}。spec 9.4
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                crow::response r(503);
                r.body = R"({"error":"session client unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto ws = compatibility_workspace();
            SessionOptions opts;
            if (auto err = parse_session_options(req, ws, opts)) return std::move(*err);

            std::string id;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                refresh_default_session_preferences_for_new_session_locked();
                id = deps.session_client->create_session(opts);
            }
            LOG_INFO("[web] compatibility /api/sessions create id=" + id + " cwd=" + ws.cwd);
            crow::response r(201);
            r.body = json{
                {"session_id", id},
                {"id", id},
                {"workspace_hash", opts.no_workspace ? std::string{} : ws.hash},
                {"cwd", opts.no_workspace ? std::string{} : ws.cwd},
                {"no_workspace", opts.no_workspace}
            }.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/resume: 把磁盘历史恢复进当前 daemon。
        CROW_ROUTE(app, "/api/sessions/<string>/resume").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                crow::response r(503);
                r.body = R"({"error":"session client unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto ws = compatibility_workspace();
            SessionOptions opts;
            opts.cwd = ws.cwd;
            opts.workspace_hash = ws.hash;
            auto project_dir = SessionStorage::get_project_dir(ws.cwd);
            auto meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, id));
            if (meta.id.empty()) {
                if (auto no_workspace_meta = find_no_workspace_session_meta(id)) {
                    meta = *no_workspace_meta;
                    opts.cwd = meta.cwd.empty()
                        ? no_workspace_session_cwd(id, no_workspace_cache_root())
                        : meta.cwd;
                    project_dir = SessionStorage::get_project_dir(opts.cwd);
                    opts.no_workspace = true;
                    opts.workspace_hash.clear();
                }
            }
            if (meta.no_workspace) {
                opts.no_workspace = true;
                if (!meta.cwd.empty()) opts.cwd = meta.cwd;
                opts.workspace_hash.clear();
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
            crow::response r(200);
            r.body = json{
                {"session_id", id},
                {"id", id},
                {"active", true},
                {"workspace_hash", opts.no_workspace ? std::string{} : ws.hash},
                {"cwd", opts.no_workspace ? std::string{} : ws.cwd},
                {"no_workspace", opts.no_workspace}
            }.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/sessions/<string>/archive").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            return set_session_archive_state(req, compatibility_workspace(), id, true);
        });

        CROW_ROUTE(app, "/api/sessions/<string>/archive").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            return set_session_archive_state(req, compatibility_workspace(), id, false);
        });

        CROW_ROUTE(app, "/api/sessions/<string>/title").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            return set_session_title_response(req, compatibility_workspace(), id);
        });

        CROW_ROUTE(app, "/api/sessions/<string>/draft").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            return get_session_input_draft(req, compatibility_workspace(), id);
        });

        CROW_ROUTE(app, "/api/sessions/<string>/draft").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            return set_session_input_draft(req, compatibility_workspace(), id);
        });

        CROW_ROUTE(app, "/api/sessions/<string>/todos").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            return clear_session_todos(req, compatibility_workspace(), id);
        });

        // DELETE /api/sessions/:id: 销毁(内存)。spec 9.5
        // ?purge=1: 永久删除磁盘数据。允许已归档主会话与 spawn_subagent
        // 子会话;普通主会话仍拒绝,运行中目标需先中止。
        // 注意: 用 Delete(混合大小写)避免 Windows <windows.h> 把 DELETE 宏化掉。
        CROW_ROUTE(app, "/api/sessions/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                return with_cors(req, crow::response(503));
            }
            const char* purge_raw = req.url_params.get("purge");
            const bool purge = purge_raw &&
                (std::string(purge_raw) == "1" || std::string(purge_raw) == "true");
            if (!purge) {
                deps.session_client->destroy_session(id);
                return with_cors(req, crow::response(204));
            }
            return purge_session_data(
                req, compatibility_workspace(), id, /*require_archived=*/false);
        });

        // GET /api/sessions/:id/messages?since=N: 拉历史 + 缓存事件。spec 9.6
        // v1: 只回放 EventDispatcher 缓存里 seq > since 的事件。完整磁盘历史
        // 由 GET /api/sessions/:id 取(若需要)。这里强调"轻量补齐",前端
        // WS 重连时 ?since=N 用同样语义。
        CROW_ROUTE(app, "/api/sessions/<string>/messages").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::uint64_t since = 0;
            if (auto s = req.url_params.get("since")) since = parse_seq(s);

            // 用 subscribe(since_seq) 触发 replay,回调里收集事件;然后立刻
            // unsubscribe。因为 EventDispatcher 的 replay 是同步的,subscribe
            // 返回前已 push 完所有补帧。
            json arr = json::array();
            std::string workspace_hash;
            std::string session_cwd;
            if (deps.session_registry) {
                if (auto entry = deps.session_registry->acquire(id)) {
                    workspace_hash = entry->workspace_hash;
                    session_cwd = entry->cwd;
                }
            }
            if (deps.session_client) {
                auto sub = deps.session_client->subscribe(id,
                    [&arr, &id, &workspace_hash, &session_cwd](const SessionEvent& e) {
                        arr.push_back(session_event_to_json(e, id, workspace_hash, session_cwd));
                    },
                    since);
                if (sub != 0) deps.session_client->unsubscribe(id, sub);
            }

            // 同时附加磁盘 ChatMessage 历史(供首次连接补齐 LLM 上下文)
            // 仅当 since=0 时返回,避免重连时重复推。
            if (since == 0 && deps.session_registry) {
                if (auto entry = deps.session_registry->acquire(id)) {
                    if (entry->sm) {
                        json msgs = json::array();
                        for (const auto& m : entry->sm->load_active_messages()) {
                            if (is_file_checkpoint_message(m)) continue;
                            if (is_compact_checkpoint_message(m)) continue;
                            if (is_hidden_goal_context_message(m)) continue;
                            msgs.push_back(chat_message_to_json(m));
                        }
                        json wrapper;
                        wrapper["events"]   = std::move(arr);
                        wrapper["messages"] = std::move(msgs);
                        append_session_runtime_snapshot(wrapper, id);
                        crow::response r(wrapper.dump());
                        r.add_header("Content-Type", "application/json");
                        return with_cors(req, std::move(r));
                    }
                }

                std::vector<std::string> project_dirs{
                    SessionStorage::get_project_dir(deps.cwd),
                };
                if (auto no_workspace_meta = find_no_workspace_session_meta(id)) {
                    if (!no_workspace_meta->cwd.empty()) {
                        const auto no_workspace_project_dir =
                            SessionStorage::get_project_dir(no_workspace_meta->cwd);
                        if (std::find(project_dirs.begin(), project_dirs.end(),
                                      no_workspace_project_dir) == project_dirs.end()) {
                            project_dirs.push_back(no_workspace_project_dir);
                        }
                    }
                }
                for (const auto& project_dir : project_dirs) {
                    auto candidates = SessionStorage::find_session_files(project_dir, id);
                    if (!candidates.empty()) {
                        json msgs = json::array();
                        for (const auto& m : SessionStorage::load_messages(candidates.front().jsonl_path)) {
                            if (is_file_checkpoint_message(m)) continue;
                            if (is_compact_checkpoint_message(m)) continue;
                            if (is_hidden_goal_context_message(m)) continue;
                            msgs.push_back(chat_message_to_json(m));
                        }
                        json wrapper;
                        wrapper["events"]   = std::move(arr);
                        wrapper["messages"] = std::move(msgs);
                        append_session_runtime_snapshot(wrapper, id);
                        crow::response r(wrapper.dump());
                        r.add_header("Content-Type", "application/json");
                        return with_cors(req, std::move(r));
                    }
                }
            }

            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/export-markdown: 选择目录并导出完整可见 transcript。
        // 目录选择和最终写盘都留在 daemon,避免浏览器下载目录权限/路径能力不足。
        CROW_ROUTE(app, "/api/sessions/<string>/export-markdown").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            auto export_error = [this, &req](int status, const std::string& message) {
                crow::response r(status);
                r.body = json{{"ok", false}, {"error", message}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };

            std::string workspace_hash_hint;
            if (!req.body.empty()) {
                try {
                    const auto body = json::parse(req.body);
                    if (!body.is_object()) return export_error(400, "bad json");
                    workspace_hash_hint = body.value("workspace_hash", std::string{});
                } catch (const std::exception& e) {
                    return export_error(400, std::string("bad json: ") + e.what());
                }
            }

            const auto workspace = resolve_session_workspace(id, workspace_hash_hint);
            if (!workspace.has_value()) return export_error(404, "workspace not found");
            auto maybe_meta = find_session_meta_for_workspace(*workspace, id);
            if (!maybe_meta.has_value()) return export_error(404, "session not found");

            SessionMeta meta = *maybe_meta;
            std::vector<ChatMessage> messages;
            if (auto entry = active_session_entry_for_workspace(*workspace, id)) {
                if (entry->sm) {
                    const auto active_meta = entry->sm->load_session_meta(id);
                    if (!active_meta.id.empty()) meta = active_meta;
                    messages = entry->sm->load_active_messages();
                }
            }
            if (messages.empty()) {
                const std::string session_cwd = meta.cwd.empty() ? workspace->cwd : meta.cwd;
                const auto project_dir = SessionStorage::get_project_dir(session_cwd);
                const auto candidates = SessionStorage::find_session_files(project_dir, id);
                if (!candidates.empty()) {
                    messages = SessionStorage::load_messages(candidates.front().jsonl_path);
                }
            }

            std::vector<ChatMessage> visible_messages;
            visible_messages.reserve(messages.size());
            for (const auto& message : messages) {
                if (is_file_checkpoint_message(message)) continue;
                if (is_compact_checkpoint_message(message)) continue;
                if (is_hidden_goal_context_message(message)) continue;
                visible_messages.push_back(message);
            }

            if (!deps.native_folder_picker_enabled) {
                return export_error(501, "native folder picker unavailable");
            }
            if (!deps.native_folder_picker) {
                return export_error(503, "native folder picker callback unavailable");
            }
            const auto picked = deps.native_folder_picker();
            if (!picked.has_value() || picked->empty()) {
                crow::response r(200);
                r.body = json{{"ok", true}, {"cancelled", true}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const auto folder = path_from_utf8(*picked);
            std::error_code ec;
            if (!std::filesystem::is_directory(folder, ec) || ec) {
                return export_error(400, "destination folder unavailable");
            }

            const std::string filename = session_export::choose_markdown_filename(
                folder, meta.title, meta.id.empty() ? id : meta.id);
            const auto output_path = folder / path_from_utf8(filename);
            const std::string markdown = session_export::build_markdown(meta, visible_messages);
            std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
            if (!output) return export_error(500, "unable to create Markdown file");
            output.write(markdown.data(), static_cast<std::streamsize>(markdown.size()));
            output.flush();
            if (!output) return export_error(500, "unable to write Markdown file");

            LOG_INFO("[web] exported session Markdown id=" + id +
                     " filename=" + filename);
            crow::response r(200);
            r.body = json{{"ok", true}, {"cancelled", false}, {"filename", filename}}.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/attachments: upload a session-scoped attachment
        // as JSON {name,mime_type,data_base64}. The message endpoint only
        // references returned attachment ids, so retries do not duplicate bytes.
        CROW_ROUTE(app, "/api/sessions/<string>/attachments").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_registry) {
                crow::response r(503);
                r.body = R"({"error":"session registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto entry = deps.session_registry->acquire(id);
            if (!entry) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            std::string name;
            std::string mime_type;
            std::string data_base64;
            try {
                auto j = json::parse(req.body);
                if (j.contains("name") && j["name"].is_string()) {
                    name = j["name"].get<std::string>();
                }
                if (j.contains("mime_type") && j["mime_type"].is_string()) {
                    mime_type = j["mime_type"].get<std::string>();
                }
                if (j.contains("data_base64") && j["data_base64"].is_string()) {
                    data_base64 = j["data_base64"].get<std::string>();
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto decoded = base64_decode(data_base64);
            if (!decoded.has_value()) {
                crow::response r(400);
                r.body = R"({"error":"invalid base64 attachment data"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const std::string project_dir = SessionStorage::get_project_dir(entry->cwd);
            std::string error;
            auto record = save_attachment(project_dir, id, name, mime_type, *decoded, &error);
            if (!record.has_value()) {
                crow::response r(400);
                r.body = json{{"error", error.empty() ? "failed to save attachment" : error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(201);
            r.body = json{{"attachment", attachment_to_json(*record)}}.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/sessions/<string>/attachments/<string>/blob").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& id, const std::string& attachment_id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_registry) {
                crow::response r(503);
                r.body = R"({"error":"session registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto entry = deps.session_registry->acquire(id);
            if (!entry) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            const std::string project_dir = SessionStorage::get_project_dir(entry->cwd);
            std::string error;
            auto record = load_attachment(project_dir, id, attachment_id, &error);
            if (!record.has_value()) {
                crow::response r(404);
                r.body = json{{"error", error.empty() ? "attachment not found" : error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto bytes = read_attachment_bytes(*record, kMaxAttachmentBytes, &error);
            if (!bytes.has_value()) {
                crow::response r(404);
                r.body = json{{"error", error.empty() ? "attachment not found" : error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(200);
            r.body = std::move(*bytes);
            r.add_header("Content-Type", record->mime_type.empty()
                ? "application/octet-stream"
                : record->mime_type);
            r.add_header("Cache-Control", "private, max-age=3600");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/messages: 向 daemon 入队用户输入。WebSocket
        // 只负责观察事件流,这样切换会话/关闭当前连接不会中断后台 AgentLoop。
        CROW_ROUTE(app, "/api/sessions/<string>/messages").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                crow::response r(503);
                r.body = R"({"error":"session client unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto parsed = parse_session_user_input_request(
                req.body, id, /*allow_worktree=*/true);
            if (!parsed.ok) {
                crow::response r(parsed.status);
                r.body = json{{"error", parsed.error}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            bool ok = deps.session_client->send_input(id, parsed.input);
            if (!ok) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(202);
            r.body = R"({"queued":true})";
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/turn/steer: append structured input to the
        // matching active regular turn. Acceptance is not transcript commit;
        // the normal user message event carrying client_message_id is the
        // durable acknowledgement.
        CROW_ROUTE(app, "/api/sessions/<string>/turn/steer").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rejection = require_auth(req)) {
                return std::move(*rejection);
            }
            if (!deps.session_client) {
                crow::response response(503);
                response.body = json{
                    {"error", "SESSION_CLIENT_UNAVAILABLE"},
                    {"message", "session client unavailable"},
                }.dump();
                response.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(response));
            }

            auto parsed = parse_session_user_input_request(
                req.body, id, /*allow_worktree=*/false);
            if (!parsed.ok) {
                crow::response response(parsed.status);
                const bool unknown_session =
                    parsed.status == 404 &&
                    parsed.error == "unknown session";
                response.body = json{
                    {"error", unknown_session
                        ? "UNKNOWN_SESSION"
                        : "INVALID_TURN_INPUT"},
                    {"message", parsed.error},
                }.dump();
                response.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(response));
            }
            if (parsed.expected_turn_id.empty()) {
                crow::response response(400);
                response.body = json{
                    {"error", "EXPECTED_TURN_ID_REQUIRED"},
                    {"message", "expected_turn_id is required"},
                }.dump();
                response.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(response));
            }

            const auto result = deps.session_client->steer_input(
                id, parsed.expected_turn_id, parsed.input);
            int status = 409;
            std::string code;
            switch (result.status) {
            case TurnSteerStatus::Accepted:
                status = 202;
                break;
            case TurnSteerStatus::InvalidInput:
                status = 400;
                code = "INVALID_TURN_INPUT";
                break;
            case TurnSteerStatus::UnknownSession:
                status = 404;
                code = "UNKNOWN_SESSION";
                break;
            case TurnSteerStatus::NoActiveTurn:
                code = "NO_ACTIVE_TURN";
                break;
            case TurnSteerStatus::NonSteerable:
                code = "TURN_NOT_STEERABLE";
                break;
            case TurnSteerStatus::TurnMismatch:
                code = "TURN_MISMATCH";
                break;
            case TurnSteerStatus::QueueFull:
                status = 429;
                code = "TURN_STEER_QUEUE_FULL";
                break;
            }

            crow::response response(status);
            if (result.accepted()) {
                json body = {
                    {"accepted", true},
                    {"turn_id", result.turn_id},
                };
                if (parsed.input.metadata.is_object()) {
                    const std::string client_message_id =
                        parsed.input.metadata.value(
                            "client_message_id", std::string{});
                    if (!client_message_id.empty()) {
                        body["client_message_id"] = client_message_id;
                    }
                }
                response.body = body.dump();
            } else {
                json body = {
                    {"error", code},
                    {"message", result.message.empty()
                        ? std::string(to_string(result.status))
                        : result.message},
                };
                if (!result.turn_id.empty()) {
                    body["active_turn_id"] = result.turn_id;
                }
                response.body = body.dump();
            }
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        });

        // POST /api/sessions/:id/commands: daemon-owned builtin slash command
        // execution. This deliberately bypasses skill expansion and ordinary
        // message submission so `/init` and `/compact` have TUI-equivalent
        // behavior in Desktop/Web.
        CROW_ROUTE(app, "/api/sessions/<string>/commands").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                crow::response r(503);
                r.body = R"({"error":"session client unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto parsed = web::parse_builtin_command_request(req.body);
            if (!parsed.ok) {
                crow::response r(parsed.status);
                r.body = web::builtin_command_error_json(parsed).dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            BuiltinCommandRequest cmd;
            cmd.name = std::move(parsed.request.name);
            cmd.args = std::move(parsed.request.args);
            cmd.display_text = std::move(parsed.request.display_text);
            auto result = deps.session_client->execute_builtin_command(id, cmd);
            if (result.status == BuiltinCommandStatus::UnknownSession) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (result.status == BuiltinCommandStatus::UnsupportedCommand) {
                crow::response r(400);
                r.body = json{{"error", "unsupported command"}, {"command", cmd.name}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (result.status == BuiltinCommandStatus::Failed) {
                crow::response r(500);
                r.body = json{{"error", result.message.empty() ? "command failed" : result.message}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            crow::response r(202);
            r.body = json{{"queued", true}, {"command", cmd.name}}.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/side-question: `/btw` one-turn side question.
        // The registry reads AgentLoop's detached provider-facing snapshot and
        // calls the current model without tools. No transcript/event/busy state
        // is mutated by this request.
        CROW_ROUTE(app, "/api/sessions/<string>/side-question").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_registry) {
                crow::response r(503);
                r.body = json{{"error", "SESSION_REGISTRY_UNAVAILABLE"},
                              {"message", "session registry unavailable"}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            std::string question;
            try {
                auto body = json::parse(req.body);
                if (!body.is_object() || !body.contains("question") ||
                    !body["question"].is_string()) {
                    crow::response r(400);
                    r.body = json{{"error", "INVALID_SIDE_QUESTION"},
                                  {"message", "question must be a string"}}.dump();
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                question = body["question"].get<std::string>();
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", "INVALID_SIDE_QUESTION"},
                              {"message", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto result = deps.session_registry->ask_side_question(id, question);
            int status = 200;
            std::string code;
            switch (result.status) {
            case SideQuestionStatus::Ok:
                break;
            case SideQuestionStatus::InvalidQuestion:
                status = 400;
                code = "INVALID_SIDE_QUESTION";
                break;
            case SideQuestionStatus::UnknownSession:
                status = 404;
                code = "UNKNOWN_SESSION";
                break;
            case SideQuestionStatus::ContextNotReady:
                status = 409;
                code = "SIDE_QUESTION_CONTEXT_NOT_READY";
                break;
            case SideQuestionStatus::ProviderUnavailable:
                status = 503;
                code = "SIDE_QUESTION_PROVIDER_UNAVAILABLE";
                break;
            case SideQuestionStatus::Failed:
                status = 502;
                code = "SIDE_QUESTION_FAILED";
                break;
            }

            crow::response r(status);
            if (result.status == SideQuestionStatus::Ok) {
                r.body = json{{"question", result.question},
                              {"answer", result.answer}}.dump();
            } else {
                r.body = json{{"error", code},
                              {"message", result.error.empty()
                                  ? "side question failed"
                                  : result.error}}.dump();
            }
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/sessions/:id/permissions: current active session permission mode.
        CROW_ROUTE(app, "/api/sessions/<string>/permissions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_registry) {
                crow::response r(503);
                r.body = R"({"error":"session registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            auto mode = deps.session_registry->permission_mode(id);
            if (!mode.has_value()) {
                auto project_dir = SessionStorage::get_project_dir(deps.cwd);
                auto meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, id));
                if (!meta.id.empty()) {
                    auto parsed = parse_permission_mode_name(meta.permission_mode);
                    crow::response r(permission_mode_to_json(parsed.value_or(PermissionMode::Default)).dump());
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            crow::response r(permission_mode_to_json(*mode).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // PUT /api/sessions/:id/permissions body {mode}: switch current session only.
        CROW_ROUTE(app, "/api/sessions/<string>/permissions").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_registry) {
                crow::response r(503);
                r.body = R"({"error":"session registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            std::string mode_name;
            try {
                auto j = json::parse(req.body);
                if (j.contains("mode") && j["mode"].is_string()) {
                    mode_name = j["mode"].get<std::string>();
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto mode = parse_permission_mode_name(mode_name);
            if (!mode.has_value()) {
                crow::response r(400);
                r.body = R"({"error":"invalid permission mode"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!deps.session_registry->set_permission_mode(id, *mode)) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            crow::response r(permission_mode_to_json(*mode).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/file-checkpoints/:message_id/restore:
        // restore workspace files to the checkpoint captured for that user turn.
        // This is code-only rewind; conversation history remains intact.
        CROW_ROUTE(app, "/api/sessions/<string>/file-checkpoints/<string>/restore").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id, const std::string& message_id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (message_id.empty()) {
                crow::response r(400);
                r.body = R"({"error":"message_id required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!deps.session_registry) {
                crow::response r(503);
                r.body = R"({"error":"session registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto entry = deps.session_registry->acquire(id);
            if (!entry || !entry->sm) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (entry->loop && entry->loop->is_busy()) {
                crow::response r(409);
                r.body = R"({"error":"session busy"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!entry->sm->file_checkpoint_can_restore(message_id)) {
                crow::response r(404);
                r.body = R"({"error":"file checkpoint not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            FileCheckpointRestoreResult result;
            try {
                result = entry->sm->rewind_files_to_checkpoint(message_id);
            } catch (const std::exception& e) {
                LOG_ERROR("[web] restore checkpoint " + id + " threw: " + e.what());
                crow::response r(500);
                r.body = json{{"error", std::string("restore failed: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            json resp;
            resp["ok"] = result.ok();
            resp["session_id"] = id;
            resp["message_id"] = message_id;
            resp["files_changed"] = result.files_changed;
            resp["errors"] = result.errors;
            crow::response r(result.ok() ? 200 : 500);
            r.body = resp.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/fork: 把 source session 截止到 at_message_id
        // (含此条)的前缀复制到一个新 session id,新 session 立即可用但不
        // 自动启动 turn。源 session 保持不动。
        // body: {at_message_id: string, title?: string}
        // 200: {session_id, title, forked_from, fork_message_id}
        // 400: at_message_id missing / message_not_found
        // 404: source session 不在 SessionRegistry
        // 500: IO 异常(已自动清理半个文件)
        CROW_ROUTE(app, "/api/sessions/<string>/fork").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string at_message_id;
            std::string explicit_title;
            try {
                auto j = json::parse(req.body);
                if (j.contains("at_message_id") && j["at_message_id"].is_string()) {
                    at_message_id = j["at_message_id"].get<std::string>();
                }
                if (j.contains("title") && j["title"].is_string()) {
                    explicit_title = j["title"].get<std::string>();
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (at_message_id.empty()) {
                crow::response r(400);
                r.body = R"({"error":"at_message_id required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            if (!deps.session_registry) {
                crow::response r(503);
                r.body = R"({"error":"session registry unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto entry = deps.session_registry->acquire(id);
            if (!entry || !entry->loop || !entry->sm) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            // Fork from the durable human transcript, not the compacted
            // effective model history held by AgentLoop.
            auto messages = entry->sm->load_active_messages();

            auto idx = find_message_index_by_id(messages, at_message_id);
            if (!idx.has_value()) {
                crow::response r(400);
                r.body = R"({"error":"message_not_found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            // 含被点击的那条:retained = msgs[0..idx]
            auto retained = retained_prefix_before_index(messages, *idx + 1);

            // 组 source meta + sibling 列表用于命名规则
            auto source_meta = entry->sm->load_session_meta(id);
            if (source_meta.id.empty()) source_meta.id = id;
            if (source_meta.title.empty()) {
                // 内存里 title 可能比磁盘新(刚改还没 update_meta)
                source_meta.title = entry->sm->current_title();
            }
            std::string project_dir = SessionStorage::get_project_dir(entry->cwd);
            auto siblings = SessionStorage::list_sessions(project_dir);

            std::string title = compute_fork_title(source_meta, siblings, explicit_title);

            // 写新 session 文件(IO 异常 fork_session_to_new_id 内部已清理)
            std::string new_id;
            try {
                new_id = entry->sm->fork_session_to_new_id(retained, title, id, at_message_id);
            } catch (const std::exception& e) {
                LOG_ERROR("[web] fork " + id + " threw: " + e.what());
                crow::response r(500);
                r.body = json{{"error", std::string("fork failed: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (new_id.empty()) {
                crow::response r(500);
                r.body = R"({"error":"fork failed"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            // 把新 session 装进 registry — 走 resume 路径(磁盘 → 内存)。
            // resume 不会自动启动 turn,符合 spec "不自动启动 turn"。
            SessionOptions resume_opts;
            resume_opts.cwd = entry->cwd;
            resume_opts.no_workspace = entry->no_workspace;
            resume_opts.workspace_hash = entry->no_workspace ? std::string{} : entry->workspace_hash;
            if (!deps.session_registry->resume(new_id, resume_opts)) {
                LOG_WARN("[web] fork: new session " + new_id +
                         " written to disk but registry resume failed");
            }

            json resp;
            resp["session_id"]      = new_id;
            resp["title"]           = title;
            resp["forked_from"]     = id;
            resp["fork_message_id"] = at_message_id;
            resp["workspace_hash"]   = entry->no_workspace ? std::string{} : entry->workspace_hash;
            resp["cwd"]              = entry->no_workspace ? std::string{} : entry->cwd;
            resp["no_workspace"]     = entry->no_workspace;
            crow::response r(resp.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
    }
} // namespace acecode::web
