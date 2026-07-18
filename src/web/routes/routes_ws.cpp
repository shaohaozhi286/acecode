// routes_ws.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

void WebServer::Impl::register_websocket() {
        CROW_WEBSOCKET_ROUTE(app, "/ws/sessions/<string>")
            .onaccept([this](const crow::request& req, void**) -> bool {
                // 鉴权: ?token=xxx 或 X-ACECode-Token (普通浏览器 WS 没法
                // 设 header,所以 query 是主路径)。跨 origin 的 Desktop/Web
                // 多 daemon 连接即便来自 loopback 也必须带 token。
                std::string header_token;
                auto h = req.get_header_value("X-ACECode-Token");
                if (!h.empty()) header_token = h;
                std::string query_token;
                if (auto t = req.url_params.get("token")) query_token = t;

                auto r = auth_result_for_request(req, header_token, query_token);
                if (r != AuthResult::Allowed) {
                    log_unauthorized(req.url, req.remote_ip_address,
                                       r == AuthResult::NoToken ? "ws no token" : "ws bad token");
                    return false;
                }
                return true;
            })
            .onopen([this](crow::websocket::connection& conn) {
                // Crow WS 的 onopen 不直接传 route 参数;走 hello-binding:
                // 客户端 onopen 后立刻发 {type:"hello", payload:{session_id, since}},
                // handle_ws_message 完成 SessionClient::subscribe 绑定。
                std::lock_guard<std::mutex> lk(ws_mu);
                ws_connections.emplace(&conn, std::make_shared<WsConnState>());
                LOG_INFO("[ws] connection opened");
            })
            .onmessage([this](crow::websocket::connection& conn,
                                const std::string& data, bool /*is_binary*/) {
                handle_ws_message(conn, data);
            })
            .onclose([this](crow::websocket::connection& conn,
                              const std::string& reason, uint16_t /*code*/) {
                handle_ws_close(conn, reason);
            });
    }

void WebServer::Impl::handle_ws_message(crow::websocket::connection& conn, const std::string& data) {
    json msg;
    try { msg = json::parse(data); }
    catch (...) {
        conn.send_text(R"({"type":"error","payload":{"reason":"bad json"}})");
        return;
    }

    auto type = msg.value("type", std::string{});
    const auto& payload = msg.contains("payload") ? msg["payload"] : json::object();

    std::shared_ptr<WsConnState> state;
    {
        std::lock_guard<std::mutex> lk(ws_mu);
        auto it = ws_connections.find(&conn);
        if (it != ws_connections.end()) state = it->second;
    }
    if (!state) {
        conn.send_text(R"({"type":"error","payload":{"reason":"no connection state"}})");
        return;
    }

    if (type == "status_subscribe") {
        auto workspace_hash = payload.value("workspace_hash", std::string{});
        auto sid = payload.value("session_id", std::string{});
        if (workspace_hash.empty() && !sid.empty()) {
            if (auto ws = resolve_session_workspace(sid)) workspace_hash = ws->hash;
        }
        if (workspace_hash.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing workspace_hash"}})");
            return;
        }
        auto ws = resolve_workspace(workspace_hash);
        if (!ws.has_value()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"unknown workspace"}})");
            return;
        }
        state->status_workspaces.insert(ws->hash);
        send_status_snapshot(conn, *ws);
        return;
    }

    if (type == "status_unsubscribe") {
        auto workspace_hash = payload.value("workspace_hash", std::string{});
        if (!workspace_hash.empty()) {
            if (auto ws = resolve_workspace(workspace_hash)) state->status_workspaces.erase(ws->hash);
            state->status_workspaces.erase(workspace_hash);
        }
        json ack;
        ack["type"] = "status_unsubscribe_ack";
        ack["payload"] = json{{"workspace_hash", workspace_hash}};
        conn.send_text(ack.dump());
        return;
    }

    if (type == "mark_session_read") {
        auto sid = payload.value("session_id", std::string{});
        if (sid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
            return;
        }
        auto workspace_hash = payload.value("workspace_hash", std::string{});
        auto ws = resolve_session_workspace(sid, workspace_hash);
        if (!ws.has_value()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"unknown workspace"}})");
            return;
        }
        const std::uint64_t cursor = payload.value("cursor", static_cast<std::uint64_t>(0));
        auto status = mark_session_read_status(sid, ws->hash, ws->cwd, cursor);
        json ack;
        ack["type"] = "mark_session_read_ack";
        ack["session_id"] = sid;
        ack["workspace_hash"] = ws->hash;
        ack["payload"] = status;
        conn.send_text(ack.dump());
        return;
    }

    if (type == "hello" || type == "subscribe") {
        auto sid = payload.value("session_id", std::string{});
        std::uint64_t since = payload.value("since", static_cast<std::uint64_t>(0));
        if (sid.empty() || !deps.session_client) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
            return;
        }

        std::string workspace_hash;
        std::string session_cwd;
        std::string parent_session_id;
        if (deps.session_registry) {
            if (auto entry = deps.session_registry->acquire(sid)) {
                workspace_hash = entry->workspace_hash;
                session_cwd = entry->cwd;
                parent_session_id = entry->parent_session_id;
            }
        }

        const auto send_subscription_ack = [&] {
            json ack;
            ack["type"]       = type == "hello" ? "hello_ack" : "subscribe_ack";
            ack["session_id"] = sid;
            if (!workspace_hash.empty()) ack["workspace_hash"] = workspace_hash;
            if (!parent_session_id.empty()) {
                ack["parent_session_id"] = parent_session_id;
            }
            ack["payload"] = json{
                {"session_id", sid},
                {"workspace_hash", workspace_hash},
                {"cwd", session_cwd},
            };
            if (!parent_session_id.empty()) {
                ack["payload"]["parent_session_id"] = parent_session_id;
            }
            conn.send_text(ack.dump());
        };

        // Parent subscription is also the discovery channel for already-running
        // children. This avoids relying on a workspace-wide unknown-session
        // heuristic and covers no-workspace parents.
        const auto send_child_status_snapshots = [&] {
            for (const auto& child : deps.session_client->list_sessions()) {
                if (child.parent_session_id != sid) continue;
                json status_payload{
                    {"session_id", child.id},
                    {"parent_session_id", sid},
                    {"workspace_hash", child.workspace_hash},
                    {"cwd", child.cwd},
                    {"busy", child.busy},
                    {"state", child.busy ? "running" : "read"},
                    {"attention_state", child.busy ? "running" : "read"},
                    {"read_state", child.busy ? "running" : "read"},
                };
                json status;
                status["type"] = "session_status";
                status["timestamp_ms"] = now_unix_ms();
                status["session_id"] = child.id;
                status["parent_session_id"] = sid;
                if (!child.workspace_hash.empty()) {
                    status["workspace_hash"] = child.workspace_hash;
                }
                status["payload"] = std::move(status_payload);
                try { conn.send_text(status.dump()); } catch (...) {}
            }
        };

        // since=0 does not replay EventDispatcher's ring. After the ack (which
        // establishes parent ownership), send seq-less snapshots for unresolved
        // permission and AskUserQuestion interactions. Frontend queues dedupe by
        // request_id and retain close-before-request tombstones.
        const auto send_pending_interaction_snapshots = [&] {
            if (!deps.session_registry) return;
            auto pending_entry = deps.session_registry->acquire(sid);
            if (!pending_entry) return;

            const auto send_snapshot =
                [&](SessionEventKind kind, nlohmann::json req) {
                    req["session_id"] = sid;
                    if (!parent_session_id.empty()) {
                        req["parent_session_id"] = parent_session_id;
                    }
                    if (!workspace_hash.empty()) {
                        req["workspace_hash"] = workspace_hash;
                    }
                    json snapshot;
                    snapshot["type"] = to_string(kind);
                    snapshot["session_id"] = sid;
                    if (!parent_session_id.empty()) {
                        snapshot["parent_session_id"] = parent_session_id;
                    }
                    if (!workspace_hash.empty()) {
                        snapshot["workspace_hash"] = workspace_hash;
                    }
                    snapshot["payload"] = std::move(req);
                    try { conn.send_text(snapshot.dump()); } catch (...) {}
                };

            if (pending_entry->prompter) {
                for (auto& req :
                     pending_entry->prompter->snapshot_pending_requests()) {
                    send_snapshot(SessionEventKind::PermissionRequest,
                                  std::move(req));
                }
            }
            if (pending_entry->ask_prompter) {
                for (auto& req :
                     pending_entry->ask_prompter->snapshot_pending_requests()) {
                    send_snapshot(SessionEventKind::QuestionRequest,
                                  std::move(req));
                }
            }
        };

        if (state->subscriptions.find(sid) != state->subscriptions.end()) {
            {
                std::lock_guard<std::mutex> lk(ws_mu);
                state->status_sessions.insert(sid);
            }
            send_subscription_ack();
            send_child_status_snapshots();
            send_pending_interaction_snapshots();
            return;
        }
        crow::websocket::connection* conn_ptr = &conn;
        std::string sid_copy = sid;
        auto sub = deps.session_client->subscribe(sid,
            [this, conn_ptr, sid_copy, workspace_hash, session_cwd](const SessionEvent& evt) {
                const auto text = session_event_to_json(evt, sid_copy, workspace_hash, session_cwd).dump();
                try {
                    std::lock_guard<std::mutex> lk(ws_mu);
                    if (ws_connections.find(conn_ptr) != ws_connections.end()) {
                        conn_ptr->send_text(text);
                    }
                } catch (...) {
                }
                note_session_event_for_attention(sid_copy, workspace_hash, session_cwd, evt);
            },
            since);
        if (sub == 0) {
            conn.send_text(R"({"type":"error","payload":{"reason":"unknown session"}})");
            return;
        }
        if (state->session_id.empty()) state->session_id = sid;
        state->subscriptions[sid] = sub;
        {
            std::lock_guard<std::mutex> lk(ws_mu);
            state->status_sessions.insert(sid);
        }
        send_subscription_ack();
        send_child_status_snapshots();
        send_pending_interaction_snapshots();
        return;
    }

    if (type == "unsubscribe") {
        auto sid = payload.value("session_id", std::string{});
        if (sid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
            return;
        }
        auto it = state->subscriptions.find(sid);
        if (it != state->subscriptions.end()) {
            if (deps.session_client) deps.session_client->unsubscribe(sid, it->second);
            state->subscriptions.erase(it);
            {
                std::lock_guard<std::mutex> lk(ws_mu);
                state->status_sessions.erase(sid);
            }
            if (state->session_id == sid) {
                state->session_id = state->subscriptions.empty()
                    ? std::string{}
                    : state->subscriptions.begin()->first;
            }
        }
        json ack;
        ack["type"]       = "unsubscribe_ack";
        ack["session_id"] = sid;
        ack["payload"]    = json{{"session_id", sid}};
        conn.send_text(ack.dump());
        return;
    }

    if (state->subscriptions.empty()) {
        conn.send_text(R"({"type":"error","payload":{"reason":"send hello first"}})");
        return;
    }

    auto target_session_id = [&]() -> std::string {
        auto sid = payload.value("session_id", std::string{});
        if (!sid.empty()) return sid;
        if (!state->session_id.empty()) return state->session_id;
        return state->subscriptions.size() == 1 ? state->subscriptions.begin()->first : std::string{};
    };

    if (type == "user_input") {
        auto text = payload.value("text", std::string{});
        auto sid = target_session_id();
        if (sid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
            return;
        }
        if (deps.session_client && !deps.session_client->send_input(sid, text)) {
            conn.send_text(R"({"type":"error","payload":{"reason":"unknown session"}})");
        }
        return;
    }
    if (type == "decision") {
        auto sid = target_session_id();
        if (sid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
            return;
        }
        auto rid = payload.value("request_id", std::string{});
        auto choice_str = payload.value("choice", std::string{"deny"});
        auto choice_opt = parse_permission_choice(choice_str);
        if (!choice_opt.has_value() || rid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"bad decision"}})");
            return;
        }
        PermissionDecision dec;
        dec.request_id = rid;
        dec.choice     = *choice_opt;
        if (deps.session_client) deps.session_client->respond_permission(sid, dec);
        return;
    }
    if (type == "abort") {
        auto sid = target_session_id();
        if (sid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
            return;
        }
        if (deps.session_client) deps.session_client->abort(sid);
        return;
    }
    if (type == "question_answer") {
        auto sid = target_session_id();
        if (sid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
            return;
        }
        auto rid = payload.value("request_id", std::string{});
        if (rid.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"bad question_answer"}})");
            return;
        }
        AskUserQuestionResponse resp;
        resp.cancelled = payload.value("cancelled", false);
        if (!resp.cancelled && payload.contains("answers") && payload["answers"].is_array()) {
            for (const auto& a : payload["answers"]) {
                if (!a.is_object()) continue;
                AskUserQuestionAnswer ans;
                ans.question_id = a.value("question_id", std::string{});
                if (a.contains("selected") && a["selected"].is_array()) {
                    for (const auto& s : a["selected"]) {
                        if (s.is_string()) ans.selected.push_back(s.get<std::string>());
                    }
                }
                ans.custom_text = a.value("custom_text", std::string{});
                resp.answers.push_back(std::move(ans));
            }
        }
        if (deps.session_client) {
            deps.session_client->respond_question(sid, rid, resp);
        }
        return;
    }
    if (type == "ping") {
        conn.send_text(R"({"type":"pong"})");
        return;
    }

    conn.send_text(json{{"type", "error"},
                        {"payload", json{{"reason", "unknown type"}, {"got", type}}}}.dump());
}

void WebServer::Impl::handle_ws_close(crow::websocket::connection& conn, const std::string& reason) {
    std::shared_ptr<WsConnState> state;
    {
        std::lock_guard<std::mutex> lk(ws_mu);
        auto it = ws_connections.find(&conn);
        if (it != ws_connections.end()) {
            state = std::move(it->second);
            ws_connections.erase(it);
        }
    }
    if (state && deps.session_client) {
        for (const auto& [sid, sub] : state->subscriptions) {
            deps.session_client->unsubscribe(sid, sub);
        }
    }
    LOG_INFO("[ws] connection closed: " + reason);
}

} // namespace acecode::web
