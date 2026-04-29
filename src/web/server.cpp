#include "server.hpp"

#include "auth.hpp"
#include "../config/config.hpp"
#include "../session/local_session_client.hpp"
#include "../session/session_client.hpp"
#include "../session/session_registry.hpp"
#include "../session/session_serializer.hpp"
#include "../session/session_storage.hpp"
#include "../skills/skill_registry.hpp"
#include "../skills/skill_metadata.hpp"
#include "../utils/logger.hpp"
#include "version.hpp"

// Crow 头一定在 ASIO_STANDALONE PUBLIC 定义之后才 include。CMakeLists.txt 已
// 给 acecode_testable 加 PUBLIC 的 ASIO_STANDALONE,所以这里直接 include 即可。
#include <crow.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::web {

namespace {

using nlohmann::json;

// 把字符串当作 SubscriptionId(uint64_t)解析。失败返回 0(EventDispatcher
// 的合法 id 从 1 开始,0 永远是非法)。
std::uint64_t parse_seq(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoull(s); } catch (...) { return 0; }
}

std::int64_t now_unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// 把 ChatMessage 序列化成网络可传 JSON。复用 session_serializer 的输出格式
// (canonical OpenAI-flavored)。HTTP /api/sessions/:id/messages 用。
json chat_message_to_json(const ChatMessage& m) {
    // serialize_message 返回一行 JSONL,我们解回 json 对象再返回。
    try {
        return json::parse(serialize_message(m));
    } catch (...) {
        return json{{"role", m.role}, {"content", m.content}};
    }
}

// SessionEvent → 上行 WS 消息(也用于 /messages 回放)。
json session_event_to_json(const SessionEvent& evt) {
    json msg;
    msg["type"]         = to_string(evt.kind);
    msg["seq"]          = evt.seq;
    msg["timestamp_ms"] = evt.timestamp_ms;
    msg["payload"]      = evt.payload;
    return msg;
}

// per-WS-connection 状态:订阅 id + session_id + abort 标志。Crow 的
// connection.userdata() 存原始指针,生命周期由 ws_connections_ map 管理。
struct WsConnState {
    std::string                       session_id;
    SessionClient::SubscriptionId     sub_id = 0;
};

// 把 client_ip / 401 reason 写日志(避免每个路由都重复)。
void log_unauthorized(const std::string& path,
                       const std::string& client_ip,
                       const char* reason) {
    LOG_WARN(std::string("[web] 401 ") + reason + " path=" + path
             + " client_ip=" + client_ip);
}

} // namespace

// =====================================================================
// Impl: 隐藏所有 Crow 类型,避免泄漏到 server.hpp(防止下游 TU 都拖 crow.h)
// =====================================================================
struct WebServer::Impl {
    WebServerDeps              deps;
    crow::SimpleApp            app;

    // ws 注册表: 把 listener / state 与 connection 绑定,断开时清理。
    std::mutex                                                      ws_mu;
    std::unordered_map<crow::websocket::connection*, std::unique_ptr<WsConnState>> ws_connections;

    explicit Impl(WebServerDeps d) : deps(std::move(d)) {}

    // -----------------------------------------------------------------
    // 鉴权 helper
    // -----------------------------------------------------------------
    // 返回 nullopt = 通过;返回 response = 拒绝(调用方 return 这个值)。
    std::optional<crow::response> require_auth(const crow::request& req) {
        std::string header_token;
        auto h = req.get_header_value("X-ACECode-Token");
        if (!h.empty()) header_token = h;
        std::string query_token;
        auto qt = req.url_params.get("token");
        if (qt) query_token = qt;

        auto result = check_request_auth(req.remote_ip_address,
                                          deps.token,
                                          header_token,
                                          query_token);
        if (result == AuthResult::Allowed) return std::nullopt;

        const char* reason = (result == AuthResult::NoToken)
                              ? "no token" : "bad token";
        log_unauthorized(req.url, req.remote_ip_address, reason);
        crow::response resp(401);
        resp.add_header("Content-Type", "application/json");
        resp.body = json{{"error", reason}}.dump();
        return resp;
    }

    // -----------------------------------------------------------------
    // 路由注册
    // -----------------------------------------------------------------
    void register_routes() {
        register_health();
        register_sessions();
        register_skills();
        register_mcp();
        register_websocket();
    }

    void register_health() {
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
            crow::response r(j.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });
    }

    void register_sessions() {
        // GET /api/sessions: 内存活跃 + 磁盘历史合并去重。spec 9.3
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            // 内存活跃
            std::vector<SessionInfo> active;
            if (deps.session_client) active = deps.session_client->list_sessions();

            // 磁盘历史
            auto project_dir = SessionStorage::get_project_dir(deps.cwd);
            auto disk = SessionStorage::list_sessions(project_dir);

            // 合并去重: id 集合
            std::unordered_set<std::string> seen;
            json arr = json::array();
            for (const auto& s : active) {
                seen.insert(s.id);
                json o;
                o["id"]            = s.id;
                o["active"]        = true;
                o["title"]         = s.title;
                o["summary"]       = s.summary;
                o["created_at"]    = s.created_at;
                o["updated_at"]    = s.updated_at;
                o["provider"]      = s.provider;
                o["model"]         = s.model;
                o["message_count"] = s.message_count;
                arr.push_back(std::move(o));
            }
            for (const auto& m : disk) {
                if (seen.count(m.id)) continue;
                json o;
                o["id"]            = m.id;
                o["active"]        = false;
                o["title"]         = m.title;
                o["summary"]       = m.summary;
                o["created_at"]    = m.created_at;
                o["updated_at"]    = m.updated_at;
                o["provider"]      = m.provider;
                o["model"]         = m.model;
                o["message_count"] = m.message_count;
                arr.push_back(std::move(o));
            }
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // POST /api/sessions: 新建 session,返回 {session_id}。spec 9.4
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                crow::response r(503);
                r.body = R"({"error":"session client unavailable"})";
                r.add_header("Content-Type", "application/json");
                return r;
            }

            SessionOptions opts;
            if (!req.body.empty()) {
                try {
                    auto j = json::parse(req.body);
                    if (j.contains("model") && j["model"].is_string())
                        opts.model_name = j["model"].get<std::string>();
                    if (j.contains("initial_user_message") && j["initial_user_message"].is_string())
                        opts.initial_user_message = j["initial_user_message"].get<std::string>();
                    if (j.contains("auto_start") && j["auto_start"].is_boolean())
                        opts.auto_start = j["auto_start"].get<bool>();
                } catch (const std::exception& e) {
                    crow::response r(400);
                    r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                    r.add_header("Content-Type", "application/json");
                    return r;
                }
            }

            auto id = deps.session_client->create_session(opts);
            crow::response r(201);
            r.body = json{{"session_id", id}}.dump();
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // DELETE /api/sessions/:id: 销毁。spec 9.5
        // 注意: 用 Delete(混合大小写)避免 Windows <windows.h> 把 DELETE 宏化掉。
        CROW_ROUTE(app, "/api/sessions/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.session_client) {
                return crow::response(503);
            }
            deps.session_client->destroy_session(id);
            return crow::response(204);
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
            if (deps.session_client) {
                auto sub = deps.session_client->subscribe(id,
                    [&arr](const SessionEvent& e) {
                        arr.push_back(session_event_to_json(e));
                    },
                    since);
                if (sub != 0) deps.session_client->unsubscribe(id, sub);
            }

            // 同时附加磁盘 ChatMessage 历史(供首次连接补齐 LLM 上下文)
            // 仅当 since=0 时返回,避免重连时重复推。
            if (since == 0 && deps.session_registry) {
                if (auto* entry = deps.session_registry->lookup(id)) {
                    if (entry->loop) {
                        json msgs = json::array();
                        for (const auto& m : entry->loop->messages()) {
                            msgs.push_back(chat_message_to_json(m));
                        }
                        json wrapper;
                        wrapper["events"]   = std::move(arr);
                        wrapper["messages"] = std::move(msgs);
                        crow::response r(wrapper.dump());
                        r.add_header("Content-Type", "application/json");
                        return r;
                    }
                }
            }

            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });
    }

    void register_skills() {
        // GET /api/skills: spec 9.7
        CROW_ROUTE(app, "/api/skills").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json arr = json::array();
            if (deps.skill_registry) {
                for (const auto& s : deps.skill_registry->list()) {
                    json o;
                    o["name"]        = s.name;
                    o["command_key"] = s.command_key;
                    o["description"] = s.description;
                    o["category"]    = s.category;
                    o["enabled"]     = true; // disabled 已经在 list 里被过滤
                    arr.push_back(std::move(o));
                }
            }
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });
    }

    void register_mcp() {
        // GET /api/mcp: 读 config 当前 mcp_servers 段。spec 9.8
        CROW_ROUTE(app, "/api/mcp").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json out = json::object();
            if (deps.app_config) {
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
                    o["validate_certificates"] = srv.validate_certificates;
                    if (!srv.ca_cert_path.empty()) o["ca_cert_path"] = srv.ca_cert_path;
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
                    cfg.validate_certificates = v.value("validate_certificates", true);
                    cfg.ca_cert_path = v.value("ca_cert_path", std::string{});
                    new_servers.emplace(it.key(), std::move(cfg));
                }
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
    }

    // -----------------------------------------------------------------
    // WebSocket: /ws/sessions/:id  (spec Section 10)
    // -----------------------------------------------------------------
    void register_websocket() {
        CROW_WEBSOCKET_ROUTE(app, "/ws/sessions/<string>")
            .onaccept([this](const crow::request& req, void**) -> bool {
                // 鉴权: ?token=xxx 或 X-ACECode-Token (普通浏览器 WS 没法
                // 设 header,所以 query 是主路径)。loopback 同样跳过。
                std::string header_token;
                auto h = req.get_header_value("X-ACECode-Token");
                if (!h.empty()) header_token = h;
                std::string query_token;
                if (auto t = req.url_params.get("token")) query_token = t;

                auto r = check_request_auth(req.remote_ip_address, deps.token,
                                              header_token, query_token);
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
                ws_connections.emplace(&conn, std::make_unique<WsConnState>());
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

    void handle_ws_message(crow::websocket::connection& conn, const std::string& data) {
        json msg;
        try { msg = json::parse(data); }
        catch (...) {
            conn.send_text(R"({"type":"error","payload":{"reason":"bad json"}})");
            return;
        }

        auto type = msg.value("type", std::string{});
        const auto& payload = msg.contains("payload") ? msg["payload"] : json::object();

        WsConnState* state = nullptr;
        {
            std::lock_guard<std::mutex> lk(ws_mu);
            auto it = ws_connections.find(&conn);
            if (it != ws_connections.end()) state = it->second.get();
        }
        if (!state) {
            conn.send_text(R"({"type":"error","payload":{"reason":"no connection state"}})");
            return;
        }

        // hello: { type:"hello", payload:{ session_id:"...", since: 0 } }
        // 浏览器端在 onopen 后立刻发 hello 完成 session 绑定 + 订阅事件流。
        if (type == "hello") {
            if (!state->session_id.empty()) {
                conn.send_text(R"({"type":"error","payload":{"reason":"already bound"}})");
                return;
            }
            auto sid = payload.value("session_id", std::string{});
            std::uint64_t since = payload.value("since", static_cast<std::uint64_t>(0));
            if (sid.empty() || !deps.session_client) {
                conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
                return;
            }
            // 订阅事件流。回调里 send_text 把事件推到浏览器。
            // 注意: callback 跑在 AgentLoop worker 线程,conn.send_text 内部
            // 加锁,Crow 保证线程安全。conn 引用在 onclose 之前都有效。
            crow::websocket::connection* conn_ptr = &conn;
            auto sub = deps.session_client->subscribe(sid,
                [conn_ptr](const SessionEvent& evt) {
                    try {
                        conn_ptr->send_text(session_event_to_json(evt).dump());
                    } catch (...) {
                        // 连接断 / 关 → send 抛,忽略(onclose 会清理)
                    }
                },
                since);
            if (sub == 0) {
                conn.send_text(R"({"type":"error","payload":{"reason":"unknown session"}})");
                return;
            }
            state->session_id = sid;
            state->sub_id     = sub;
            json ack;
            ack["type"]    = "hello_ack";
            ack["payload"] = json{{"session_id", sid}};
            conn.send_text(ack.dump());
            return;
        }

        // 没绑 session 不允许其它操作
        if (state->session_id.empty()) {
            conn.send_text(R"({"type":"error","payload":{"reason":"send hello first"}})");
            return;
        }

        if (type == "user_input") {
            auto text = payload.value("text", std::string{});
            if (deps.session_client) deps.session_client->send_input(state->session_id, text);
            return;
        }
        if (type == "decision") {
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
            if (deps.session_client) deps.session_client->respond_permission(state->session_id, dec);
            return;
        }
        if (type == "abort") {
            if (deps.session_client) deps.session_client->abort(state->session_id);
            return;
        }
        if (type == "ping") {
            conn.send_text(R"({"type":"pong"})");
            return;
        }

        conn.send_text(json{{"type", "error"},
                            {"payload", json{{"reason", "unknown type"}, {"got", type}}}}.dump());
    }

    void handle_ws_close(crow::websocket::connection& conn, const std::string& reason) {
        std::unique_ptr<WsConnState> state;
        {
            std::lock_guard<std::mutex> lk(ws_mu);
            auto it = ws_connections.find(&conn);
            if (it != ws_connections.end()) {
                state = std::move(it->second);
                ws_connections.erase(it);
            }
        }
        if (state && !state->session_id.empty() && state->sub_id != 0
            && deps.session_client) {
            deps.session_client->unsubscribe(state->session_id, state->sub_id);
        }
        LOG_INFO("[ws] connection closed: " + reason);
    }
};

// =====================================================================
// WebServer 公开方法
// =====================================================================
WebServer::WebServer(WebServerDeps deps)
    : impl_(std::make_unique<Impl>(std::move(deps))) {
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
        // Crow 在 bind 失败时抛 asio 异常,message 里通常含 "address already in
        // use" 之类。打两条日志: 一条原始错误,一条用户视角的处理建议(端口占用
        // 是最常见的失败原因)。daemon 不 retry / 不 fallback — fail-fast 退出
        // 让用户主动改 web.port 或 kill 占用端口的进程。
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
