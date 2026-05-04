#include "server.hpp"

#include "auth.hpp"
#include "static_assets.hpp"
#include "../config/config.hpp"
#include "../desktop/workspace_registry.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/llm_provider.hpp"
#include "../provider/provider_swap.hpp"
#include "../session/ask_user_question_prompter.hpp"
#include "../session/local_session_client.hpp"
#include "../session/session_attention.hpp"
#include "../session/session_client.hpp"
#include "../session/session_registry.hpp"
#include "../session/session_rewind.hpp"
#include "../session/session_serializer.hpp"
#include "../session/session_storage.hpp"
#include "../skills/skill_registry.hpp"
#include "../skills/skill_metadata.hpp"
#include "../utils/logger.hpp"
#include "../utils/cwd_hash.hpp"
#include "handlers/files_handler.hpp"
#include "handlers/fork_handler.hpp"
#include "handlers/history_handler.hpp"
#include "handlers/models_handler.hpp"
#include "handlers/pinned_sessions_handler.hpp"
#include "handlers/skills_handler.hpp"
#include "message_payload.hpp"
#include "version.hpp"

// Crow 头一定在 ASIO_STANDALONE PUBLIC 定义之后才 include。CMakeLists.txt 已
// 给 acecode_testable 加 PUBLIC 的 ASIO_STANDALONE,所以这里直接 include 即可。
#include <crow.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
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

bool cwd_is_directory(const std::string& cwd) {
    if (cwd.empty()) return false;
    std::error_code ec;
#ifdef _WIN32
    return std::filesystem::is_directory(std::filesystem::u8path(cwd), ec) && !ec;
#else
    return std::filesystem::is_directory(cwd, ec) && !ec;
#endif
}

// 把 ChatMessage 序列化成网络可传 JSON。复用 web/message_payload.cpp 的纯函数,
// 顶层带稳定 id 字段(user 走持久 UUID,其它角色 lazy sha1)。
json chat_message_to_json(const ChatMessage& m) {
    return chat_message_to_payload_json(m);
}

// SessionEvent → 上行 WS 消息(也用于 /messages 回放)。
json session_event_to_json(const SessionEvent& evt,
                           const std::string& session_id = {},
                           const std::string& workspace_hash = {},
                           const std::string& cwd = {}) {
    json msg;
    msg["type"]         = to_string(evt.kind);
    msg["seq"]          = evt.seq;
    msg["timestamp_ms"] = evt.timestamp_ms;
    msg["payload"]      = evt.payload;
    if (!session_id.empty()) {
        msg["session_id"] = session_id;
        if (msg["payload"].is_object() && !msg["payload"].contains("session_id")) {
            msg["payload"]["session_id"] = session_id;
        }
    }
    if (!workspace_hash.empty()) {
        msg["workspace_hash"] = workspace_hash;
        if (msg["payload"].is_object() && !msg["payload"].contains("workspace_hash")) {
            msg["payload"]["workspace_hash"] = workspace_hash;
        }
    }
    if (!cwd.empty()) {
        if (msg["payload"].is_object() && !msg["payload"].contains("cwd")) {
            msg["payload"]["cwd"] = cwd;
        }
    }
    return msg;
}

// per-WS-connection 状态:一个 WebSocket 可同时订阅多个 session。session_id
// 保留为 legacy primary session,兼容老的 hello/user_input/abort 消息。
struct WsConnState {
    std::string session_id;
    std::unordered_map<std::string, SessionClient::SubscriptionId> subscriptions;
    std::unordered_set<std::string> status_workspaces;
    std::unordered_set<std::string> status_sessions;
};

// 把 client_ip / 401 reason 写日志(避免每个路由都重复)。
void log_unauthorized(const std::string& path,
                       const std::string& client_ip,
                       const char* reason) {
    LOG_WARN(std::string("[web] 401 ") + reason + " path=" + path
             + " client_ip=" + client_ip);
}

bool is_loopback_origin(const std::string& origin) {
    return origin.rfind("http://127.0.0.1:", 0) == 0 ||
           origin.rfind("http://localhost:", 0) == 0 ||
           origin.rfind("http://[::1]:", 0) == 0 ||
           origin.rfind("https://127.0.0.1:", 0) == 0 ||
           origin.rfind("https://localhost:", 0) == 0 ||
           origin.rfind("https://[::1]:", 0) == 0;
}

std::string ascii_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

struct OriginParts {
    std::string scheme;
    std::string host;
    std::string port;
};

OriginParts split_host_port(std::string hostport) {
    OriginParts out;
    hostport = ascii_lower(std::move(hostport));
    if (!hostport.empty() && hostport.front() == '[') {
        auto end = hostport.find(']');
        if (end != std::string::npos) {
            out.host = hostport.substr(1, end - 1);
            if (end + 1 < hostport.size() && hostport[end + 1] == ':') {
                out.port = hostport.substr(end + 2);
            }
            return out;
        }
    }

    auto colon = hostport.rfind(':');
    if (colon != std::string::npos &&
        hostport.find(':') == colon) {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    } else {
        out.host = hostport;
    }
    return out;
}

OriginParts parse_origin(const std::string& origin) {
    OriginParts out;
    auto scheme_pos = origin.find("://");
    if (scheme_pos == std::string::npos) return out;
    out.scheme = ascii_lower(origin.substr(0, scheme_pos));
    auto rest = origin.substr(scheme_pos + 3);
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    OriginParts hp = split_host_port(rest);
    out.host = std::move(hp.host);
    out.port = std::move(hp.port);
    if (out.port.empty()) {
        if (out.scheme == "http") out.port = "80";
        else if (out.scheme == "https") out.port = "443";
    }
    return out;
}

bool is_loopback_host(const std::string& host) {
    auto h = ascii_lower(host);
    return h == "localhost" || h == "::1" ||
           h == "127.0.0.1" ||
           (h.size() > 4 && h.substr(0, 4) == "127.");
}

bool is_same_request_origin(const crow::request& req,
                            const std::string& origin) {
    auto host = req.get_header_value("Host");
    if (host.empty()) return false;
    if (origin == ("http://" + host) || origin == ("https://" + host)) {
        return true;
    }

    // Treat loopback aliases on the same port as same request origin. This
    // keeps http://127.0.0.1:36000 and ws://localhost:36000 from becoming a
    // false "cross-origin" hop that requires a Desktop token.
    auto o = parse_origin(origin);
    auto h = split_host_port(host);
    if (h.port.empty()) h.port = (o.scheme == "https") ? "443" : "80";
    return !o.host.empty() && !h.host.empty() &&
           o.port == h.port &&
           is_loopback_host(o.host) &&
           is_loopback_host(h.host);
}

AuthResult check_explicit_token(std::string_view server_token,
                                std::string_view header_token,
                                std::string_view query_token) {
    if (header_token.empty() && query_token.empty()) return AuthResult::NoToken;
    if (!server_token.empty() &&
        (header_token == server_token || query_token == server_token)) {
        return AuthResult::Allowed;
    }
    return AuthResult::BadToken;
}

} // namespace

// =====================================================================
// Impl: 隐藏所有 Crow 类型,避免泄漏到 server.hpp(防止下游 TU 都拖 crow.h)
// =====================================================================
struct WebServer::Impl {
    WebServerDeps              deps;
    crow::SimpleApp            app;

    // 静态资源 source(EmbeddedAssetSource / FileSystemAssetSource),按
    // web.static_dir 路径在 register_routes 前实例化。
    std::unique_ptr<AssetSource> assets;

    // ws 注册表: 把 listener / state 与 connection 绑定,断开时清理。
    std::mutex                                                      ws_mu;
    std::unordered_map<crow::websocket::connection*, std::unique_ptr<WsConnState>> ws_connections;

    mutable std::mutex attention_mu;
    mutable std::unordered_set<std::string> loaded_attention_workspaces;
    mutable std::unordered_map<std::string, std::string> attention_workspace_cwds;
    mutable std::unordered_map<std::string, std::unordered_map<std::string, SessionAttentionRecord>> attention_by_workspace;

    explicit Impl(WebServerDeps d) : deps(std::move(d)) {}

    // -----------------------------------------------------------------
    // 鉴权 helper
    // -----------------------------------------------------------------
    AuthResult auth_result_for_request(const crow::request& req,
                                       const std::string& header_token,
                                       const std::string& query_token) const {
        // Cross-origin browser access is only for local Desktop/Web multi-daemon
        // hops. Same-origin browser requests keep the existing loopback behavior.
        auto origin = req.get_header_value("Origin");
        if (!origin.empty() && !is_same_request_origin(req, origin)) {
            if (!is_loopback_origin(origin)) return AuthResult::BadToken;
            return check_explicit_token(deps.token, header_token, query_token);
        }
        return check_request_auth(req.remote_ip_address, deps.token,
                                  header_token, query_token);
    }

    // 返回 nullopt = 通过;返回 response = 拒绝(调用方 return 这个值)。
    std::optional<crow::response> require_auth(const crow::request& req) {
        std::string header_token;
        auto h = req.get_header_value("X-ACECode-Token");
        if (!h.empty()) header_token = h;
        std::string query_token;
        auto qt = req.url_params.get("token");
        if (qt) query_token = qt;

        auto result = auth_result_for_request(req, header_token, query_token);
        if (result == AuthResult::Allowed) return std::nullopt;

        const char* reason = (result == AuthResult::NoToken)
                              ? "no token" : "bad token";
        log_unauthorized(req.url, req.remote_ip_address, reason);
        crow::response resp(401);
        resp.add_header("Content-Type", "application/json");
        resp.body = json{{"error", reason}}.dump();
        add_cors(req, resp);
        return resp;
    }

    void add_cors(const crow::request& req, crow::response& resp) {
        std::string origin = req.get_header_value("Origin");
        if (origin.empty() || !is_loopback_origin(origin)) return;
        resp.add_header("Access-Control-Allow-Origin", origin);
        resp.add_header("Vary", "Origin");
        resp.add_header("Access-Control-Allow-Credentials", "false");
        resp.add_header("Access-Control-Allow-Headers", "Content-Type, X-ACECode-Token");
        resp.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    }

    crow::response with_cors(const crow::request& req, crow::response resp) {
        add_cors(req, resp);
        return resp;
    }

    crow::response cors_preflight(const crow::request& req) {
        crow::response r(204);
        add_cors(req, r);
        return r;
    }

    std::string projects_dir() const {
        if (!deps.projects_dir.empty()) return deps.projects_dir;
        return (std::filesystem::path(get_acecode_dir()) / "projects").string();
    }

    acecode::desktop::WorkspaceMeta compatibility_workspace() const {
        acecode::desktop::WorkspaceMeta m;
        m.cwd = deps.cwd;
        m.hash = compute_cwd_hash(deps.cwd);
        m.name = acecode::desktop::default_workspace_name(deps.cwd);
        return m;
    }

    std::optional<acecode::desktop::WorkspaceMeta> resolve_workspace(const std::string& hash) const {
        if (hash == "__local__") return compatibility_workspace();
        if (deps.workspace_registry) {
            if (auto m = deps.workspace_registry->get(hash)) {
                return m;
            }
            return std::nullopt;
        }
        auto compat = compatibility_workspace();
        if (hash == compat.hash) return compat;
        return std::nullopt;
    }

    std::vector<std::string> allowed_file_cwds() const {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& cwd) {
            if (cwd.empty() || seen.count(cwd)) return;
            seen.insert(cwd);
            out.push_back(cwd);
        };

        add(deps.cwd);
        if (deps.workspace_registry) {
            for (const auto& m : deps.workspace_registry->list()) {
                add(m.cwd);
            }
        }
        return out;
    }

    json workspace_to_json(const acecode::desktop::WorkspaceMeta& m) const {
        json o;
        o["hash"] = m.hash;
        o["cwd"] = m.cwd;
        o["name"] = m.name;
        o["available"] = cwd_is_directory(m.cwd);
        return o;
    }

    json session_info_to_json(const SessionInfo& s, const SessionMeta* m) const {
        json o;
        o["id"]            = s.id;
        o["active"]        = true;
        o["status"]        = s.busy ? "running" : "idle";
        o["workspace_hash"] = !s.workspace_hash.empty() ? s.workspace_hash : (m ? compute_cwd_hash(m->cwd) : "");
        o["cwd"]           = !s.cwd.empty() ? s.cwd : (m ? m->cwd : "");
        o["title"]         = !s.title.empty() ? s.title : (m ? m->title : "");
        o["summary"]       = !s.summary.empty() ? s.summary : (m ? m->summary : "");
        o["created_at"]    = !s.created_at.empty() ? s.created_at : (m ? m->created_at : "");
        o["updated_at"]    = !s.updated_at.empty() ? s.updated_at : (m ? m->updated_at : "");
        o["provider"]      = !s.provider.empty() ? s.provider : (m ? m->provider : "");
        o["model"]         = !s.model.empty() ? s.model : (m ? m->model : "");
        o["message_count"] = s.message_count > 0 ? s.message_count : (m ? m->message_count : 0);
        append_attention_fields(o, s.id, o.value("workspace_hash", std::string{}),
                                o.value("cwd", std::string{}), s.busy);
        return o;
    }

    json session_meta_to_json(const SessionMeta& m, const std::string& workspace_hash) const {
        json o;
        o["id"]             = m.id;
        o["active"]         = false;
        o["status"]         = "idle";
        o["workspace_hash"] = workspace_hash;
        o["cwd"]            = m.cwd;
        o["title"]          = m.title;
        o["summary"]        = m.summary;
        o["created_at"]     = m.created_at;
        o["updated_at"]     = m.updated_at;
        o["provider"]       = m.provider;
        o["model"]          = m.model;
        o["message_count"]  = m.message_count;
        append_attention_fields(o, m.id, workspace_hash, m.cwd, false);
        return o;
    }

    json sessions_for_workspace(const acecode::desktop::WorkspaceMeta& ws) const {
        std::vector<SessionInfo> active;
        if (deps.session_client) active = deps.session_client->list_sessions();

        auto project_dir = SessionStorage::get_project_dir(ws.cwd);
        auto disk = SessionStorage::list_sessions(project_dir);

        std::unordered_map<std::string, SessionMeta> disk_by_id;
        for (const auto& m : disk) {
            disk_by_id[m.id] = m;
        }

        std::unordered_set<std::string> seen;
        json arr = json::array();
        for (const auto& s : active) {
            if (s.workspace_hash != ws.hash) continue;
            seen.insert(s.id);
            auto meta_it = disk_by_id.find(s.id);
            const SessionMeta* m = meta_it == disk_by_id.end() ? nullptr : &meta_it->second;
            arr.push_back(session_info_to_json(s, m));
        }
        for (const auto& m : disk) {
            if (seen.count(m.id)) continue;
            arr.push_back(session_meta_to_json(m, ws.hash));
        }
        return arr;
    }

    std::filesystem::path pinned_sessions_path_for_cwd(const std::string& cwd) const {
        return std::filesystem::path(SessionStorage::get_project_dir(cwd)) /
               "pinned_sessions.json";
    }

    std::vector<std::string> session_ids_for_workspace(
        const acecode::desktop::WorkspaceMeta& ws) const {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& id) {
            if (id.empty() || seen.count(id)) return;
            seen.insert(id);
            out.push_back(id);
        };

        if (deps.session_client) {
            for (const auto& s : deps.session_client->list_sessions()) {
                const bool same_workspace = s.workspace_hash == ws.hash ||
                    (s.workspace_hash.empty() && s.cwd == ws.cwd);
                if (same_workspace) add(s.id);
            }
        }

        auto project_dir = SessionStorage::get_project_dir(ws.cwd);
        for (const auto& m : SessionStorage::list_sessions(project_dir)) {
            add(m.id);
        }
        return out;
    }

    json pinned_sessions_to_json(const acecode::desktop::WorkspaceMeta& ws,
                                 const std::vector<std::string>& session_ids) const {
        return json{{"workspace_hash", ws.hash}, {"cwd", ws.cwd}, {"session_ids", session_ids}};
    }

    std::string attention_store_path_for_cwd(const std::string& cwd) const {
        return (std::filesystem::path(SessionStorage::get_project_dir(cwd)) /
                "session_read_state.json").string();
    }

    void load_attention_workspace_locked(const std::string& workspace_hash,
                                         const std::string& cwd) const {
        if (workspace_hash.empty()) return;
        attention_workspace_cwds[workspace_hash] = cwd;
        if (loaded_attention_workspaces.count(workspace_hash)) return;
        loaded_attention_workspaces.insert(workspace_hash);

        std::ifstream in(attention_store_path_for_cwd(cwd));
        if (!in) return;
        try {
            json root = json::parse(in, nullptr, true, true);
            if (!root.contains("sessions") || !root["sessions"].is_object()) return;
            const auto& sessions = root["sessions"];
            auto& records = attention_by_workspace[workspace_hash];
            for (auto it = sessions.begin(); it != sessions.end(); ++it) {
                if (!it.value().is_object()) continue;
                SessionAttentionRecord r;
                r.read_cursor = it.value().value("read_cursor", static_cast<std::uint64_t>(0));
                r.update_cursor = it.value().value("update_cursor", static_cast<std::uint64_t>(0));
                r.updated_at_ms = it.value().value("updated_at_ms", static_cast<std::int64_t>(0));
                records[it.key()] = r;
            }
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[web] failed to load session read state: ") + e.what());
        }
    }

    void save_attention_workspace_locked(const std::string& workspace_hash) const {
        auto cwd_it = attention_workspace_cwds.find(workspace_hash);
        if (cwd_it == attention_workspace_cwds.end() || cwd_it->second.empty()) return;

        json root;
        root["version"] = 1;
        root["sessions"] = json::object();
        const auto records_it = attention_by_workspace.find(workspace_hash);
        if (records_it != attention_by_workspace.end()) {
            for (const auto& [sid, record] : records_it->second) {
                root["sessions"][sid] = json{
                    {"read_cursor", record.read_cursor},
                    {"update_cursor", record.update_cursor},
                    {"updated_at_ms", record.updated_at_ms},
                };
            }
        }

        const auto path = std::filesystem::path(attention_store_path_for_cwd(cwd_it->second));
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const auto tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out << root.dump(2);
            out << '\n';
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(tmp, path, ec);
        }
        if (ec) {
            LOG_WARN("[web] failed to save session read state: " + ec.message());
        }
    }

    SessionAttentionRecord attention_record_for_session(const std::string& workspace_hash,
                                                        const std::string& cwd,
                                                        const std::string& session_id,
                                                        bool busy) const {
        std::lock_guard<std::mutex> lk(attention_mu);
        load_attention_workspace_locked(workspace_hash, cwd);
        auto record = attention_by_workspace[workspace_hash][session_id];
        record.busy = busy;
        return record;
    }

    json attention_payload_for_record(const std::string& session_id,
                                      const std::string& workspace_hash,
                                      const std::string& cwd,
                                      const SessionAttentionRecord& record) const {
        const auto state = session_attention_state_for(record);
        json payload;
        payload["session_id"] = session_id;
        payload["workspace_hash"] = workspace_hash;
        if (!cwd.empty()) payload["cwd"] = cwd;
        payload["state"] = to_string(state);
        payload["attention_state"] = to_string(state);
        payload["read_state"] = to_string(state);
        payload["busy"] = record.busy;
        payload["cursor"] = record.update_cursor;
        payload["update_cursor"] = record.update_cursor;
        payload["read_cursor"] = record.read_cursor;
        payload["timestamp_ms"] = record.updated_at_ms > 0 ? record.updated_at_ms : now_unix_ms();
        return payload;
    }

    void append_attention_fields(json& o,
                                 const std::string& session_id,
                                 const std::string& workspace_hash,
                                 const std::string& cwd,
                                 bool busy) const {
        auto record = attention_record_for_session(workspace_hash, cwd, session_id, busy);
        auto payload = attention_payload_for_record(session_id, workspace_hash, cwd, record);
        o["attention_state"] = payload["attention_state"];
        o["read_state"] = payload["read_state"];
        o["busy"] = payload["busy"];
        o["status_cursor"] = payload["cursor"];
        o["update_cursor"] = payload["update_cursor"];
        o["read_cursor"] = payload["read_cursor"];
    }

    std::optional<acecode::desktop::WorkspaceMeta> resolve_session_workspace(
        const std::string& session_id,
        const std::string& workspace_hash_hint = {}) const {
        if (!session_id.empty() && deps.session_registry) {
            if (auto* entry = deps.session_registry->lookup(session_id)) {
                acecode::desktop::WorkspaceMeta ws;
                ws.hash = !entry->workspace_hash.empty() ? entry->workspace_hash : compute_cwd_hash(entry->cwd);
                ws.cwd = entry->cwd;
                ws.name = acecode::desktop::default_workspace_name(entry->cwd);
                return ws;
            }
        }
        if (!workspace_hash_hint.empty()) {
            if (auto ws = resolve_workspace(workspace_hash_hint)) return ws;
        }
        return compatibility_workspace();
    }

    void broadcast_session_status(const json& payload) {
        json msg;
        msg["type"] = "session_status";
        msg["timestamp_ms"] = now_unix_ms();
        msg["session_id"] = payload.value("session_id", std::string{});
        msg["workspace_hash"] = payload.value("workspace_hash", std::string{});
        msg["payload"] = payload;
        const auto text = msg.dump();

        std::lock_guard<std::mutex> lk(ws_mu);
        for (const auto& [conn, state] : ws_connections) {
            if (!state) continue;
            const auto workspace_hash = payload.value("workspace_hash", std::string{});
            const auto session_id = payload.value("session_id", std::string{});
            const bool wants_workspace = !workspace_hash.empty() && state->status_workspaces.count(workspace_hash);
            const bool wants_session = !session_id.empty() && state->status_sessions.count(session_id);
            if (!wants_workspace && !wants_session) continue;
            try { conn->send_text(text); } catch (...) {}
        }
    }

    void note_session_event_for_attention(const std::string& session_id,
                                          const std::string& workspace_hash,
                                          const std::string& cwd,
                                          const SessionEvent& evt) {
        if (session_id.empty() || workspace_hash.empty()) return;
        json payload;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(attention_mu);
            load_attention_workspace_locked(workspace_hash, cwd);
            auto& record = attention_by_workspace[workspace_hash][session_id];
            const auto before_state = session_attention_state_for(record);
            const auto before_record = record;
            const auto cursor = evt.timestamp_ms > 0
                ? static_cast<std::uint64_t>(evt.timestamp_ms)
                : evt.seq;
            record = apply_session_attention_event(record, evt.kind, evt.payload, cursor, evt.timestamp_ms);
            const auto after_state = session_attention_state_for(record);
            changed = before_state != after_state || before_record.busy != record.busy;
            if (record.read_cursor != before_record.read_cursor ||
                record.update_cursor != before_record.update_cursor ||
                record.updated_at_ms != before_record.updated_at_ms ||
                record.busy != before_record.busy) {
                save_attention_workspace_locked(workspace_hash);
            }
            if (changed) payload = attention_payload_for_record(session_id, workspace_hash, cwd, record);
        }
        if (changed) broadcast_session_status(payload);
    }

    json mark_session_read_status(const std::string& session_id,
                                  const std::string& workspace_hash,
                                  const std::string& cwd,
                                  std::uint64_t cursor) {
        json payload;
        bool changed = false;
        bool current_busy = false;
        if (deps.session_registry) {
            if (auto* entry = deps.session_registry->lookup(session_id)) {
                current_busy = entry->loop && entry->loop->is_busy();
            }
        }
        {
            std::lock_guard<std::mutex> lk(attention_mu);
            load_attention_workspace_locked(workspace_hash, cwd);
            auto& record = attention_by_workspace[workspace_hash][session_id];
            record.busy = current_busy;
            const auto before_state = session_attention_state_for(record);
            const auto before_record = record;
            record = mark_session_attention_read(record, cursor, now_unix_ms());
            const auto after_state = session_attention_state_for(record);
            changed = before_state != after_state || before_record.read_cursor != record.read_cursor;
            if (changed || before_record.updated_at_ms != record.updated_at_ms) {
                save_attention_workspace_locked(workspace_hash);
            }
            payload = attention_payload_for_record(session_id, workspace_hash, cwd, record);
        }
        if (changed) broadcast_session_status(payload);
        return payload;
    }

    void send_status_snapshot(crow::websocket::connection& conn,
                              const acecode::desktop::WorkspaceMeta& ws) {
        json sessions = json::array();
        for (const auto& s : sessions_for_workspace(ws)) {
            json item;
            item["session_id"] = s.value("id", std::string{});
            item["workspace_hash"] = s.value("workspace_hash", ws.hash);
            item["cwd"] = s.value("cwd", ws.cwd);
            item["state"] = s.value("attention_state", std::string("read"));
            item["attention_state"] = item["state"];
            item["read_state"] = s.value("read_state", item["state"].get<std::string>());
            item["busy"] = s.value("busy", false);
            item["cursor"] = s.value("status_cursor", static_cast<std::uint64_t>(0));
            item["update_cursor"] = s.value("update_cursor", static_cast<std::uint64_t>(0));
            item["read_cursor"] = s.value("read_cursor", static_cast<std::uint64_t>(0));
            sessions.push_back(std::move(item));
        }
        json msg;
        msg["type"] = "session_status_snapshot";
        msg["timestamp_ms"] = now_unix_ms();
        msg["workspace_hash"] = ws.hash;
        msg["payload"] = json{{"workspace_hash", ws.hash}, {"sessions", sessions}};
        conn.send_text(msg.dump());
    }

    std::optional<crow::response> parse_session_options(
        const crow::request& req,
        const acecode::desktop::WorkspaceMeta& ws,
        SessionOptions& opts) {
        opts.cwd = ws.cwd;
        opts.workspace_hash = ws.hash;
        if (req.body.empty()) return std::nullopt;
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
            return with_cors(req, std::move(r));
        }
        return std::nullopt;
    }

    void apply_workspace_model_override(
        const acecode::desktop::WorkspaceMeta& ws,
        SessionOptions& opts) {
        if (opts.model_name.empty()) {
            if (auto cwd_override = load_cwd_model_override(ws.cwd)) {
                opts.model_name = *cwd_override;
                LOG_INFO("[web] cwd model override resolved hash=" + ws.hash +
                         " model=" + opts.model_name);
            }
        }
        if (opts.model_name.empty()) return;

        if (!deps.app_config || !deps.provider || !deps.provider_mu) return;
        auto entry = find_model_by_name(*deps.app_config, opts.model_name);
        if (!entry.has_value()) {
            LOG_WARN("[web] model override ignored for hash=" + ws.hash +
                     ": unknown model " + opts.model_name);
            return;
        }
        // Provider remains daemon-global for now; resolving here keeps cwd
        // overrides tied to the workspace cwd that requested session creation.
        swap_provider_if_needed(*deps.provider, *deps.provider_mu,
                                *entry, *deps.app_config);
    }

    // -----------------------------------------------------------------
    // 路由注册
    // -----------------------------------------------------------------
    void register_routes() {
        register_health();
        register_workspaces();
        register_pinned_sessions();
        register_sessions();
        register_models();
        register_history();
        register_files();
        register_skills();
        register_mcp();
        register_websocket();
        register_static();
    }

    void register_workspaces() {
        CROW_ROUTE(app, "/api/workspaces").methods(crow::HTTPMethod::Options)
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
            auto arr = sessions_for_workspace(*ws);
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
            apply_workspace_model_override(*ws, opts);
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
            if (!deps.session_client->resume_session(id, opts)) {
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
    }

    void register_static() {
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

    void register_history() {
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
            auto arr = load_history(cwd, max, deps.app_config->input_history);
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
            append_history(deps.cwd, text, deps.app_config->input_history);
            return with_cors(req, crow::response(204));
        });
    }

    void register_pinned_sessions() {
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

    // SidePanel "文件" / "预览" tab 的后端。共享 daemon 下,文件浏览允许
    // daemon cwd + registry 中显式注册的 desktop workspace cwd。未显式注册
    // 的 hidden TUI cwd 不会出现在 registry list 里,仍不会被 Desktop 浏览。
    void register_files() {
        CROW_ROUTE(app, "/api/files").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/files/content").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // 把 FileError 序列化为 (status_code, json) — 共用给两个端点。
        // err.message 透出到 body.detail 便于浏览器 network tab 排查;不会泄露
        // 系统路径以外的敏感信息(全是 caller 自己传的 cwd/path)。
        auto error_response = [this](const crow::request& req,
                                      const FileError& err) -> crow::response {
            crow::response r;
            r.add_header("Content-Type", "application/json");
            json body;
            switch (err.kind) {
                case FileErrorKind::UnknownWorkspace:
                    r.code = 400;
                    body["error"] = "unknown workspace";
                    break;
                case FileErrorKind::PathOutsideWorkspace:
                    r.code = 400;
                    body["error"] = "path outside workspace";
                    break;
                case FileErrorKind::NotFound:
                    r.code = 404;
                    body["error"] = "not found";
                    break;
                case FileErrorKind::TooLarge:
                    r.code = 415;
                    body["error"] = "file too large";
                    body["size"]  = err.size;
                    break;
                case FileErrorKind::Binary:
                    r.code = 415;
                    body["error"] = "binary";
                    break;
                case FileErrorKind::IoError:
                default:
                    r.code = 500;
                    body["error"] = "io error";
                    break;
            }
            if (!err.message.empty()) body["detail"] = err.message;
            r.body = body.dump();
            return with_cors(req, std::move(r));
        };

        // GET /api/files?cwd=<abs>&path=<rel>&show_hidden=<0|1>
        CROW_ROUTE(app, "/api/files").methods(crow::HTTPMethod::GET)
        ([this, error_response](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string path_q;
            bool show_hidden = false;
            if (auto c = req.url_params.get("cwd"))         cwd_q = c;
            if (auto p = req.url_params.get("path"))        path_q = p;
            if (auto s = req.url_params.get("show_hidden")) {
                std::string v = s;
                show_hidden = (v == "1" || v == "true");
            }
            if (cwd_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd parameter required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto allowed_cwds = allowed_file_cwds();
            auto validated = validate_path_within(cwd_q, path_q, allowed_cwds);
            if (std::holds_alternative<FileError>(validated)) {
                return error_response(req, std::get<FileError>(validated));
            }
            auto abs_target = std::get<std::filesystem::path>(validated);
            auto abs_cwd_v = validate_path_within(cwd_q, "", allowed_cwds);
            // abs_cwd_v 不可能失败(同样的 cwd 才到这一步);保险起见 fallback
            std::filesystem::path abs_cwd =
                std::holds_alternative<std::filesystem::path>(abs_cwd_v)
                    ? std::get<std::filesystem::path>(abs_cwd_v)
                    : std::filesystem::path(cwd_q);

            auto listed = list_directory(abs_target, abs_cwd, show_hidden);
            if (std::holds_alternative<FileError>(listed)) {
                return error_response(req, std::get<FileError>(listed));
            }
            auto& entries = std::get<std::vector<FileEntry>>(listed);
            json arr = json::array();
            for (const auto& e : entries) {
                json item;
                item["name"] = e.name;
                item["path"] = e.path;
                item["kind"] = e.kind;
                if (e.size.has_value())        item["size"]        = *e.size;
                if (e.modified_ms.has_value()) item["modified_ms"] = *e.modified_ms;
                arr.push_back(std::move(item));
            }
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/files/content?cwd=<abs>&path=<rel>
        CROW_ROUTE(app, "/api/files/content").methods(crow::HTTPMethod::GET)
        ([this, error_response](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string path_q;
            if (auto c = req.url_params.get("cwd"))  cwd_q  = c;
            if (auto p = req.url_params.get("path")) path_q = p;
            if (cwd_q.empty() || path_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd and path parameters required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto allowed_cwds = allowed_file_cwds();
            auto validated = validate_path_within(cwd_q, path_q, allowed_cwds);
            if (std::holds_alternative<FileError>(validated)) {
                return error_response(req, std::get<FileError>(validated));
            }
            auto abs_file = std::get<std::filesystem::path>(validated);

            auto content = read_file_content(abs_file);
            if (std::holds_alternative<FileError>(content)) {
                return error_response(req, std::get<FileError>(content));
            }
            crow::response r(std::get<std::string>(content));
            r.add_header("Content-Type", "text/plain; charset=utf-8");
            r.add_header("Cache-Control", "no-cache");
            return with_cors(req, std::move(r));
        });
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
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::Options)
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
        CROW_ROUTE(app, "/api/sessions/<string>/resume").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/fork").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });

        // GET /api/sessions: 内存活跃 + 磁盘历史合并去重。spec 9.3
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            LOG_INFO("[web] compatibility /api/sessions list for cwd=" + deps.cwd);
            auto arr = sessions_for_workspace(compatibility_workspace());
            crow::response r(arr.dump());
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
            apply_workspace_model_override(ws, opts);

            auto id = deps.session_client->create_session(opts);
            LOG_INFO("[web] compatibility /api/sessions create id=" + id + " cwd=" + ws.cwd);
            crow::response r(201);
            r.body = json{{"session_id", id}, {"id", id}, {"workspace_hash", ws.hash}, {"cwd", ws.cwd}}.dump();
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
            if (!deps.session_client->resume_session(id, opts)) {
                crow::response r(404);
                r.body = R"({"error":"session not found"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            crow::response r(200);
            r.body = json{{"session_id", id}, {"id", id}, {"active", true}, {"workspace_hash", ws.hash}, {"cwd", ws.cwd}}.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
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
            return with_cors(req, crow::response(204));
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
                if (auto* entry = deps.session_registry->lookup(id)) {
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
                        return with_cors(req, std::move(r));
                    }
                }

                auto project_dir = SessionStorage::get_project_dir(deps.cwd);
                auto candidates = SessionStorage::find_session_files(project_dir, id);
                if (!candidates.empty()) {
                    json msgs = json::array();
                    for (const auto& m : SessionStorage::load_messages(candidates.front().jsonl_path)) {
                        if (is_file_checkpoint_message(m)) continue;
                        msgs.push_back(chat_message_to_json(m));
                    }
                    json wrapper;
                    wrapper["events"]   = std::move(arr);
                    wrapper["messages"] = std::move(msgs);
                    crow::response r(wrapper.dump());
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
            }

            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
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

            std::string text;
            try {
                auto j = json::parse(req.body);
                if (j.contains("text") && j["text"].is_string()) {
                    text = j["text"].get<std::string>();
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (text.empty()) {
                crow::response r(400);
                r.body = R"({"error":"text required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (!deps.session_client->send_input(id, text)) {
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

            auto* entry = deps.session_registry->lookup(id);
            if (!entry || !entry->loop || !entry->sm) {
                crow::response r(404);
                r.body = R"({"error":"unknown session"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            // 拷一份 messages(后续算 prefix 不动 entry->loop 内部)
            auto messages = entry->loop->messages();

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
            resume_opts.workspace_hash = entry->workspace_hash;
            if (!deps.session_registry->resume(new_id, resume_opts)) {
                LOG_WARN("[web] fork: new session " + new_id +
                         " written to disk but registry resume failed");
            }

            json resp;
            resp["session_id"]      = new_id;
            resp["title"]           = title;
            resp["forked_from"]     = id;
            resp["fork_message_id"] = at_message_id;
            resp["workspace_hash"]   = entry->workspace_hash;
            resp["cwd"]              = entry->cwd;
            crow::response r(resp.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
    }

    void register_models() {
        CROW_ROUTE(app, "/api/models").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/sessions/<string>/model").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) {
            return cors_preflight(req);
        });

        // GET /api/models: 返回 saved_models + 合成 (legacy) 行
        CROW_ROUTE(app, "/api/models").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            auto arr = list_models(*deps.app_config);
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/sessions/:id/model body {name}: 切当前 effective model
        CROW_ROUTE(app, "/api/sessions/<string>/model").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& sid) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
            if (!deps.session_registry) return crow::response(503);
            if (!deps.provider || !deps.provider_mu) {
                crow::response r(503);
                r.body = R"({"error":"provider state unavailable"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

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

            // 真正切换。注: v1 切的是 daemon 全局 provider,所有 session 共享一个
            // provider —— 切完后所有 session 下一轮 turn 都会用新模型。文档要明示。
            swap_provider_if_needed(*deps.provider, *deps.provider_mu,
                                      *entry, *deps.app_config);

            crow::response r(200);
            r.body = json{
                {"name",            entry->name},
                {"context_window",  deps.app_config->context_window},
            }.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
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
                // 同时把 cfg.skills.disabled 中的条目也列出(状态=false),让前端
                // 看见所有可切换的 skill。
                if (deps.app_config) {
                    for (const auto& name : deps.app_config->skills.disabled) {
                        json o;
                        o["name"]        = name;
                        o["command_key"] = name;
                        o["description"] = "";
                        o["category"]    = "";
                        o["enabled"]     = false;
                        arr.push_back(std::move(o));
                    }
                }
            }
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // PUT /api/skills/<name> body {enabled: bool}: 切启停
        CROW_ROUTE(app, "/api/skills/<string>").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& name) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config || !deps.skill_registry) return crow::response(503);

            bool enabled;
            try {
                auto j = json::parse(req.body);
                if (!j.contains("enabled") || !j["enabled"].is_boolean()) {
                    crow::response r(400);
                    r.body = R"({"error":"enabled (boolean) required"})";
                    r.add_header("Content-Type", "application/json");
                    return r;
                }
                enabled = j["enabled"].get<bool>();
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return r;
            }

            auto result = set_skill_enabled(name, enabled,
                                              *deps.app_config,
                                              *deps.skill_registry,
                                              deps.config_path);
            crow::response r(result.http_status);
            r.body = result.body.dump();
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // GET /api/skills/<name>/body: 返回 SKILL.md 全文(含 frontmatter)
        CROW_ROUTE(app, "/api/skills/<string>/body").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& name) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.skill_registry) return crow::response(503);
            auto body = get_skill_body(name, *deps.skill_registry);
            if (!body.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"skill not found"})";
                r.add_header("Content-Type", "application/json");
                return r;
            }
            crow::response r(*body);
            r.add_header("Content-Type", "text/markdown; charset=utf-8");
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

        // hello/subscribe: { type, payload:{ session_id:"...", since: 0 } }
        // hello 是 legacy 单会话绑定;subscribe 可在同一 WS 上多次调用。
        if (type == "hello" || type == "subscribe") {
            auto sid = payload.value("session_id", std::string{});
            std::uint64_t since = payload.value("since", static_cast<std::uint64_t>(0));
            if (sid.empty() || !deps.session_client) {
                conn.send_text(R"({"type":"error","payload":{"reason":"missing session_id"}})");
                return;
            }
            if (state->subscriptions.find(sid) != state->subscriptions.end()) {
                std::string workspace_hash;
                std::string session_cwd;
                if (deps.session_registry) {
                    if (auto* entry = deps.session_registry->lookup(sid)) {
                        workspace_hash = entry->workspace_hash;
                        session_cwd = entry->cwd;
                    }
                }
                json ack;
                ack["type"]       = type == "hello" ? "hello_ack" : "subscribe_ack";
                ack["session_id"] = sid;
                if (!workspace_hash.empty()) ack["workspace_hash"] = workspace_hash;
                ack["payload"]    = json{{"session_id", sid}, {"workspace_hash", workspace_hash}, {"cwd", session_cwd}};
                conn.send_text(ack.dump());
                return;
            }
            // 订阅事件流。回调里 send_text 把事件推到浏览器。
            // 注意: callback 跑在 AgentLoop worker 线程,conn.send_text 内部
            // 加锁,Crow 保证线程安全。conn 引用在 onclose 之前都有效。
            crow::websocket::connection* conn_ptr = &conn;
            std::string sid_copy = sid;
            std::string workspace_hash;
            std::string session_cwd;
            if (deps.session_registry) {
                if (auto* entry = deps.session_registry->lookup(sid)) {
                    workspace_hash = entry->workspace_hash;
                    session_cwd = entry->cwd;
                }
            }
            auto sub = deps.session_client->subscribe(sid,
                [this, conn_ptr, sid_copy, workspace_hash, session_cwd](const SessionEvent& evt) {
                    try {
                        conn_ptr->send_text(session_event_to_json(evt, sid_copy, workspace_hash, session_cwd).dump());
                    } catch (...) {
                        // 连接断 / 关 → send 抛,忽略(onclose 会清理)
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
            json ack;
            ack["type"]       = type == "hello" ? "hello_ack" : "subscribe_ack";
            ack["session_id"] = sid;
            if (!workspace_hash.empty()) ack["workspace_hash"] = workspace_hash;
            ack["payload"]    = json{{"session_id", sid}, {"workspace_hash", workspace_hash}, {"cwd", session_cwd}};
            conn.send_text(ack.dump());
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

        // 没绑 session 不允许其它操作
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
            // Payload 形态参见 spec web-daemon/spec.md "AskUserQuestion 双向异步交互":
            //   { request_id, cancelled?, answers:[{question_id, selected:[...], custom_text?}] }
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
        if (state && deps.session_client) {
            for (const auto& [sid, sub] : state->subscriptions) {
                deps.session_client->unsubscribe(sid, sub);
            }
        }
        LOG_INFO("[ws] connection closed: " + reason);
    }
};

// =====================================================================
// WebServer 公开方法
// =====================================================================
WebServer::WebServer(WebServerDeps deps)
    : impl_(std::make_unique<Impl>(std::move(deps))) {
    // make_asset_source 在 web.static_dir 不存在时抛 — 这里 catch 一次,
    // 让 worker.cpp::run() 通过 "assets 为空" 的 503 返回路径感知问题(虽然
    // 对生产用户来说更早 fail-fast 也行,但 v1 选择延迟到 run() 起 server)。
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
