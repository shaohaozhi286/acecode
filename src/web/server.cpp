#include "server.hpp"

#include "auth.hpp"
#include "origin.hpp"
#include "static_assets.hpp"
#include "../auth/github_auth.hpp"
#include "../config/config.hpp"
#include "../config/saved_models_editor.hpp"
#include "../config/request_headers.hpp"
#include "../desktop/workspace_registry.hpp"
#include "../provider/llm_provider.hpp"
#include "../session/ask_user_question_prompter.hpp"
#include "../session/attachment_store.hpp"
#include "../session/local_session_client.hpp"
#include "../session/session_attention.hpp"
#include "../session/session_client.hpp"
#include "../session/session_registry.hpp"
#include "../session/session_rewind.hpp"
#include "../session/session_serializer.hpp"
#include "../session/session_storage.hpp"
#include "../session/todo_state.hpp"
#include "../session/session_usage_ledger.hpp"
#include "../session/session_writer_lease.hpp"
#include "../daemon/platform.hpp"
#include "../skills/skill_registry.hpp"
#include "../skills/skill_metadata.hpp"
#include "../tool/ace_browser_bridge/browser_tools.hpp"
#include "../tool/tool_executor.hpp"
#include "../upgrade/apply.hpp"
#include "../upgrade/check.hpp"
#include "../utils/logger.hpp"
#include "../utils/base64.hpp"
#include "../utils/cwd_hash.hpp"
#include "handlers/files_handler.hpp"
#include "handlers/fork_handler.hpp"
#include "handlers/history_handler.hpp"
#include "handlers/models_handler.hpp"
#include "handlers/permission_mode_handler.hpp"
#include "handlers/pinned_sessions_handler.hpp"
#include "handlers/builtin_command_handler.hpp"
#include "handlers/commands_handler.hpp"
#include "handlers/skill_command_expander.hpp"
#include "handlers/skills_handler.hpp"
#include "../skills/skill_init.hpp"
#include "message_payload.hpp"
#include "version.hpp"

// Crow 头一定在 ASIO_STANDALONE PUBLIC 定义之后才 include。CMakeLists.txt 已
// 给 acecode_testable 加 PUBLIC 的 ASIO_STANDALONE,所以这里直接 include 即可。
#include <crow.h>

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "../network/proxy_resolver.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef DELETE
#undef DELETE
#endif
#ifdef GET
#undef GET
#endif
#ifdef POST
#undef POST
#endif
#ifdef PUT
#undef PUT
#endif

namespace acecode::web {

namespace {

using nlohmann::json;

// 把字符串当作 SubscriptionId(uint64_t)解析。失败返回 0(EventDispatcher
// 的合法 id 从 1 开始,0 永远是非法)。
std::uint64_t parse_seq(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoull(s); } catch (...) { return 0; }
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::optional<std::string> preview_image_mime(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) return std::nullopt;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp")  return "image/bmp";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "svg")  return "image/svg+xml";
    return std::nullopt;
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

json ui_preferences_to_json(const WebUiPreferencesConfig&) {
    return json{{"show_acecode_avatar", false}};
}

json upgrade_config_to_json(const UpgradeConfig& cfg) {
    return json{{"base_url", normalize_upgrade_base_url(cfg.base_url)}};
}

json update_check_to_json(const acecode::upgrade::UpdateCheckResult& result) {
    json out = {
        {"status", acecode::upgrade::update_check_status_name(result.status)},
        {"update_available", result.update_available()},
        {"current_version", result.current_version},
        {"latest_version", result.latest_version},
        {"target", result.target},
        {"manifest_url", result.manifest_url},
    };
    if (!result.package_file.empty()) out["package_file"] = result.package_file;
    if (!result.package_url.empty()) out["package_url"] = result.package_url;
    if (result.package_size) out["package_size"] = *result.package_size;
    if (result.http_status != 0) out["http_status"] = result.http_status;
    if (!result.error.empty()) out["error"] = result.error;
    return out;
}

bool start_default_update_command(std::string* error) {
    auto exe = acecode::upgrade::current_executable_path("");
    if (exe.empty()) {
        if (error) *error = "cannot resolve acecode executable path";
        return false;
    }

#ifdef _WIN32
    std::string cmd = acecode::upgrade::quote_command_arg(exe.string()) + " update";
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    std::string cwd = exe.parent_path().string();
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &si,
        &pi);
    if (!ok) {
        if (error) *error = "failed to launch acecode update";
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = "failed to fork acecode update";
        return false;
    }
    if (pid == 0) {
        (void)::setsid();
        std::string exe_s = exe.string();
        ::execl(exe_s.c_str(), exe_s.c_str(), "update", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
#endif
}

json ace_browser_bridge_settings_to_json(const AceBrowserBridgeConfig& cfg) {
    json out;
    out["enabled"] = cfg.enabled;
    out["tool_mode"] = cfg.tool_mode;
    out["default_mode"] = cfg.default_mode;
    out["pointer_speed"] = cfg.pointer_speed;
    out["status_cache_ttl_ms"] = cfg.status_cache_ttl_ms;
    out["tool_timeout_ms"] = cfg.tool_timeout_ms;
    out["os_pointer_enabled"] = cfg.os_pointer_enabled;
    out["tab_group_enabled"] = cfg.tab_group_enabled;
    out["operation_overlay_enabled"] = cfg.operation_overlay_enabled;
    out["operation_overlay_watchdog_ms"] = cfg.operation_overlay_watchdog_ms;
    out["pointer_custom"] = {
        {"move_duration_ms_min", cfg.pointer_custom.move_duration_ms_min},
        {"move_duration_ms_max", cfg.pointer_custom.move_duration_ms_max},
        {"click_hold_ms_min", cfg.pointer_custom.click_hold_ms_min},
        {"click_hold_ms_max", cfg.pointer_custom.click_hold_ms_max},
        {"typing_delay_ms_min", cfg.pointer_custom.typing_delay_ms_min},
        {"typing_delay_ms_max", cfg.pointer_custom.typing_delay_ms_max},
        {"jitter_px", cfg.pointer_custom.jitter_px},
        {"max_path_points", cfg.pointer_custom.max_path_points},
    };
    return out;
}

constexpr std::size_t kMaxSelectionContextChars = 40000;

bool has_non_whitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

std::string json_string_field(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_string()) {
        return {};
    }
    return object[key].get<std::string>();
}

int json_positive_int_field(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return 0;
    const auto& value = object[key];
    if (value.is_number_integer()) {
        const int number = value.get<int>();
        return number > 0 ? number : 0;
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<unsigned int>();
        return number > 0 ? static_cast<int>(number) : 0;
    }
    return 0;
}

std::string truncate_selection_context_text(std::string text) {
    if (text.size() <= kMaxSelectionContextChars) return text;
    text.resize(kMaxSelectionContextChars);
    text += "\n[Selection truncated]";
    return text;
}

std::string selection_line_suffix(const json& source) {
    const int start = json_positive_int_field(source, "start_line");
    const int end = json_positive_int_field(source, "end_line");
    if (start <= 0) return {};
    if (end <= 0 || end == start) return ":" + std::to_string(start);
    return ":" + std::to_string(start) + "-" + std::to_string(end);
}

std::optional<json> sanitized_selection_context_meta(const json& ctx) {
    if (!ctx.is_object() || json_string_field(ctx, "type") != "selection") {
        return std::nullopt;
    }
    const std::string text = json_string_field(ctx, "text");
    if (!has_non_whitespace(text)) return std::nullopt;

    json source = json::object();
    if (ctx.contains("source") && ctx["source"].is_object()) {
        const auto& raw_source = ctx["source"];
        const std::string path = json_string_field(raw_source, "path");
        const std::string kind = json_string_field(raw_source, "kind");
        if (!path.empty()) source["path"] = path;
        if (!kind.empty()) source["kind"] = kind;
        const int start = json_positive_int_field(raw_source, "start_line");
        const int end = json_positive_int_field(raw_source, "end_line");
        const int line_count = json_positive_int_field(raw_source, "line_count");
        if (start > 0) source["start_line"] = start;
        if (end > 0) source["end_line"] = end;
        if (line_count > 0) source["line_count"] = line_count;
    }

    json meta = json::object();
    meta["type"] = "selection";
    const std::string id = json_string_field(ctx, "id");
    const std::string label = json_string_field(ctx, "label");
    const std::string note = json_string_field(ctx, "note");
    if (!id.empty()) meta["id"] = id;
    if (!label.empty()) meta["label"] = label;
    if (!note.empty()) meta["note"] = note;
    if (!source.empty()) meta["source"] = std::move(source);
    return meta;
}

struct SelectionPromptContext {
    json meta = json::array();
    std::string prompt;
};

SelectionPromptContext build_selection_prompt_context(const json& contexts) {
    SelectionPromptContext out;
    if (!contexts.is_array()) return out;

    std::ostringstream body;
    int count = 0;
    for (const auto& ctx : contexts) {
        auto meta = sanitized_selection_context_meta(ctx);
        if (!meta.has_value()) continue;
        std::string text = truncate_selection_context_text(json_string_field(ctx, "text"));
        if (!has_non_whitespace(text)) continue;

        ++count;
        out.meta.push_back(*meta);
        const json source = meta->contains("source") && (*meta)["source"].is_object()
            ? (*meta)["source"]
            : json::object();
        std::string source_label = json_string_field(source, "path");
        if (!source_label.empty()) {
            source_label += selection_line_suffix(source);
        } else {
            source_label = json_string_field(*meta, "label");
        }
        body << "[selection " << count << "]\n";
        if (!source_label.empty()) {
            body << "Source: " << source_label << "\n";
        }
        body << "Text:\n" << text << "\n\n";
    }

    if (count > 0) {
        out.prompt =
            "The user pinned the following selected text as reference context. "
            "Use it when it is relevant to the request.\n\n" + body.str();
    }
    return out;
}

std::string build_selection_augmented_prompt(const SelectionPromptContext& selection,
                                             const std::string& original_text) {
    std::ostringstream out;
    out << selection.prompt << "User request:\n";
    if (original_text.empty()) {
        out << "(no additional typed prompt)";
    } else {
        out << original_text;
    }
    return out.str();
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
        return path_to_utf8(path_from_utf8(get_acecode_dir()) / "projects");
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

    bool archived_query_requested(const crow::request& req) const {
        auto raw = req.url_params.get("archived");
        if (!raw) return false;
        const std::string value = ascii_lower(raw);
        return value == "1" || value == "true" || value == "yes";
    }

    UsageLedgerQuery usage_query_from_request(const crow::request& req) const {
        UsageLedgerQuery query;
        if (auto raw_days = req.url_params.get("days")) {
            try {
                query.days = std::stoi(raw_days);
            } catch (...) {
                query.days = 30;
            }
        }
        if (auto raw_workspace = req.url_params.get("workspace")) {
            query.workspace_hash = raw_workspace;
        }
        if (auto raw_tz = req.url_params.get("timezone_offset_minutes")) {
            try {
                query.timezone_offset_minutes = std::clamp(std::stoi(raw_tz), -1440, 1440);
            } catch (...) {
                query.timezone_offset_minutes = 0;
            }
        }
        return query;
    }

    std::vector<UsageLedgerScope> usage_scopes_for_request(
        const std::string& workspace_hash) const {
        std::vector<UsageLedgerScope> scopes;
        std::unordered_set<std::string> seen;
        auto add = [&](const acecode::desktop::WorkspaceMeta& ws) {
            if (!workspace_hash.empty() &&
                workspace_hash != "__local__" &&
                ws.hash != workspace_hash) {
                return;
            }
            if (ws.hash.empty() || seen.count(ws.hash)) return;
            seen.insert(ws.hash);
            scopes.push_back(UsageLedgerScope{
                SessionStorage::get_project_dir(ws.cwd),
                ws.hash,
                ws.name.empty() ? acecode::desktop::default_workspace_name(ws.cwd) : ws.name,
                ws.cwd,
            });
        };

        if (workspace_hash == "__local__") {
            add(compatibility_workspace());
            return scopes;
        }

        add(compatibility_workspace());
        if (deps.workspace_registry) {
            deps.workspace_registry->scan(projects_dir());
            for (const auto& ws : deps.workspace_registry->list()) {
                add(ws);
            }
        }
        return scopes;
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

    static bool token_usage_has_values(const TokenUsage& usage) {
        return usage.has_data ||
               usage.prompt_tokens != 0 ||
               usage.completion_tokens != 0 ||
               usage.total_tokens != 0 ||
               usage.cache_read_tokens != 0 ||
               usage.cache_write_tokens != 0 ||
               usage.reasoning_tokens != 0;
    }

    static json token_usage_to_json(const TokenUsage& usage) {
        return json{
            {"prompt_tokens", usage.prompt_tokens},
            {"completion_tokens", usage.completion_tokens},
            {"total_tokens", usage.total_tokens},
            {"cache_read_tokens", usage.cache_read_tokens},
            {"cache_write_tokens", usage.cache_write_tokens},
            {"reasoning_tokens", usage.reasoning_tokens},
            {"has_data", usage.has_data},
        };
    }

    static json token_usage_or_null(const TokenUsage& usage) {
        return token_usage_has_values(usage) ? token_usage_to_json(usage) : json(nullptr);
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
        o["model_name"]    = !s.model_name.empty() ? s.model_name : (m ? m->model_preset : "");
        o["model_preset"]  = o["model_name"];
        o["context_window"] = s.context_window;
        o["message_count"] = s.message_count > 0 ? s.message_count : (m ? m->message_count : 0);
        o["turn_count"]    = s.turn_count > 0 ? s.turn_count : (m ? m->turn_count : 0);
        o["permission_mode"] = !s.permission_mode.empty()
            ? s.permission_mode
            : (m ? m->permission_mode : "default");
        o["token_usage"] = token_usage_or_null(
            token_usage_has_values(s.last_token_usage)
                ? s.last_token_usage
                : (m ? m->last_token_usage : TokenUsage{}));
        o["session_token_usage"] = token_usage_or_null(
            token_usage_has_values(s.session_token_usage)
                ? s.session_token_usage
                : (m ? m->session_token_usage : TokenUsage{}));
        if (m && !m->todos.empty()) {
            o["todos"] = todo_items_to_json(m->todos);
            o["todo_summary"] = todo_summary_to_json(m->todos);
        }
        o["archived"]      = m ? m->archived : false;
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
        o["model_name"]     = m.model_preset;
        o["model_preset"]   = m.model_preset;
        o["message_count"]  = m.message_count;
        o["turn_count"]     = m.turn_count;
        o["permission_mode"] = m.permission_mode.empty() ? "default" : m.permission_mode;
        o["token_usage"] = token_usage_or_null(m.last_token_usage);
        o["session_token_usage"] = token_usage_or_null(m.session_token_usage);
        if (!m.todos.empty()) {
            o["todos"] = todo_items_to_json(m.todos);
            o["todo_summary"] = todo_summary_to_json(m.todos);
        }
        o["archived"]       = m.archived;
        append_attention_fields(o, m.id, workspace_hash, m.cwd, false);
        return o;
    }

    void append_session_runtime_snapshot(json& wrapper,
                                         const std::string& session_id) const {
        if (session_id.empty()) return;
        SessionMeta meta;
        bool have_meta = false;
        if (deps.session_registry) {
            if (auto* entry = deps.session_registry->lookup(session_id)) {
                if (entry->loop) {
                    wrapper["busy"] = entry->loop->is_busy();
                }
                if (entry->sm) {
                    meta = entry->sm->load_session_meta(session_id);
                    have_meta = !meta.id.empty();
                    wrapper["turn_count"] = entry->sm->current_turn_count();
                    wrapper["permission_mode"] = entry->sm->current_permission_mode();
                    wrapper["token_usage"] = token_usage_or_null(entry->sm->current_last_token_usage());
                    wrapper["session_token_usage"] =
                        token_usage_or_null(entry->sm->current_session_token_usage());
                    auto todos = entry->sm->current_todos();
                    if (!todos.empty()) {
                        wrapper["todos"] = todo_items_to_json(todos);
                        wrapper["todo_summary"] = todo_summary_to_json(todos);
                    }
                }
            }
        }

        if (!have_meta) {
            auto project_dir = SessionStorage::get_project_dir(deps.cwd);
            auto meta_path = SessionStorage::meta_path(project_dir, session_id);
            meta = SessionStorage::read_meta(meta_path);
            have_meta = !meta.id.empty();
        }
        if (have_meta) {
            if (!wrapper.contains("turn_count")) wrapper["turn_count"] = meta.turn_count;
            if (!wrapper.contains("permission_mode")) {
                wrapper["permission_mode"] = meta.permission_mode.empty() ? "default" : meta.permission_mode;
            }
            if (!wrapper.contains("token_usage")) {
                wrapper["token_usage"] = token_usage_or_null(meta.last_token_usage);
            }
            if (!wrapper.contains("session_token_usage")) {
                wrapper["session_token_usage"] = token_usage_or_null(meta.session_token_usage);
            }
            if (!wrapper.contains("todos") && !meta.todos.empty()) {
                wrapper["todos"] = todo_items_to_json(meta.todos);
                wrapper["todo_summary"] = todo_summary_to_json(meta.todos);
            }
        }

        if (!deps.session_registry) return;
        auto* entry = deps.session_registry->lookup(session_id);
        if (!entry) return;

        if (entry->sm) {
            if (auto* store = entry->sm->existing_goal_store()) {
                std::string error;
                auto goal = store->get_thread_goal(session_id, &error);
                if (!error.empty()) {
                    LOG_WARN("[web] failed to snapshot thread goal: " + error);
                } else {
                    wrapper["goal"] = goal.has_value()
                        ? thread_goal_to_json(*goal)
                        : nlohmann::json(nullptr);
                }
            }
        }
    }

    json sessions_for_workspace(const acecode::desktop::WorkspaceMeta& ws,
                                bool archived_only = false) const {
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
            const bool archived = m ? m->archived : false;
            if (archived != archived_only) continue;
            arr.push_back(session_info_to_json(s, m));
        }
        for (const auto& m : disk) {
            if (seen.count(m.id)) continue;
            if (m.archived != archived_only) continue;
            arr.push_back(session_meta_to_json(m, ws.hash));
        }
        return arr;
    }

    bool session_entry_matches_workspace(const SessionEntry& entry,
                                         const acecode::desktop::WorkspaceMeta& ws) const {
        if (!entry.workspace_hash.empty()) return entry.workspace_hash == ws.hash;
        return entry.cwd == ws.cwd;
    }

    std::optional<SessionMeta> find_session_meta_for_workspace(
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id) const {
        if (id.empty()) return std::nullopt;

        const auto project_dir = SessionStorage::get_project_dir(ws.cwd);
        const auto direct_meta_path = SessionStorage::meta_path(project_dir, id);
        std::error_code ec;
        if (std::filesystem::is_regular_file(path_from_utf8(direct_meta_path), ec)) {
            auto meta = SessionStorage::read_meta(direct_meta_path);
            if (!meta.id.empty()) return meta;
        }

        const auto candidates = SessionStorage::find_session_files(project_dir, id);
        if (!candidates.empty() && !candidates.front().meta_path.empty()) {
            auto meta = SessionStorage::read_meta(candidates.front().meta_path);
            if (!meta.id.empty()) return meta;
        }

        if (deps.session_registry) {
            if (auto* entry = deps.session_registry->lookup(id)) {
                if (!session_entry_matches_workspace(*entry, ws)) return std::nullopt;
                const auto now = SessionStorage::now_iso8601();
                SessionMeta meta;
                meta.id = id;
                meta.cwd = entry->cwd.empty() ? ws.cwd : entry->cwd;
                meta.created_at = now;
                meta.updated_at = now;
                meta.provider = entry->provider;
                meta.model = entry->model;
                meta.model_preset = entry->model_state.name;
                if (entry->sm) {
                    meta.title = entry->sm->current_title();
                    meta.input_draft = entry->sm->current_input_draft();
                }
                return meta;
            }
        }

        return std::nullopt;
    }

    crow::response set_session_archive_state(
        const crow::request& req,
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id,
        bool archived) {
        auto maybe_meta = find_session_meta_for_workspace(ws, id);
        if (!maybe_meta.has_value()) {
            crow::response r(404);
            r.body = R"({"error":"session not found"})";
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        }

        SessionMeta meta = *maybe_meta;
        if (archived && deps.session_client) {
            deps.session_client->destroy_session(id);
            const auto reread = find_session_meta_for_workspace(ws, id);
            if (reread.has_value()) meta = *reread;
        } else if (!archived && deps.session_registry) {
            if (auto* entry = deps.session_registry->lookup(id)) {
                if (session_entry_matches_workspace(*entry, ws) && entry->sm) {
                    entry->sm->set_session_archived(false);
                    const auto reread = find_session_meta_for_workspace(ws, id);
                    if (reread.has_value()) meta = *reread;
                }
            }
        }

        meta.archived = archived;
        const auto project_dir = SessionStorage::get_project_dir(ws.cwd);
        SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);

        crow::response r(session_meta_to_json(meta, ws.hash).dump());
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    }

    crow::response session_input_draft_response(
        const crow::request& req,
        const std::string& id,
        const std::string& text) {
        crow::response r(json{{"session_id", id}, {"id", id}, {"text", text}}.dump());
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    }

    crow::response session_todos_response(
        const crow::request& req,
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id,
        const std::vector<TodoItem>& todos) {
        crow::response r(json{
            {"session_id", id},
            {"id", id},
            {"workspace_hash", ws.hash},
            {"todos", todo_items_to_json(todos)},
            {"todo_summary", todo_summary_to_json(todos)},
        }.dump());
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    }

    std::optional<crow::response> parse_session_input_draft_request(
        const crow::request& req,
        std::string& text) {
        try {
            auto j = json::parse(req.body);
            if (!j.contains("text") || !j["text"].is_string()) {
                crow::response r(400);
                r.body = R"({"error":"text required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            text = j["text"].get<std::string>();
            return std::nullopt;
        } catch (const std::exception& e) {
            crow::response r(400);
            r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        }
    }

    SessionEntry* active_session_entry_for_workspace(
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id) const {
        if (!deps.session_registry) return nullptr;
        auto* entry = deps.session_registry->lookup(id);
        if (!entry || !session_entry_matches_workspace(*entry, ws)) return nullptr;
        return entry;
    }

    crow::response get_session_input_draft(
        const crow::request& req,
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id) {
        if (auto* entry = active_session_entry_for_workspace(ws, id)) {
            const std::string text = entry->sm ? entry->sm->current_input_draft() : std::string{};
            return session_input_draft_response(req, id, text);
        }

        auto maybe_meta = find_session_meta_for_workspace(ws, id);
        if (!maybe_meta.has_value()) {
            crow::response r(404);
            r.body = R"({"error":"session not found"})";
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        }
        return session_input_draft_response(req, id, maybe_meta->input_draft);
    }

    crow::response set_session_input_draft(
        const crow::request& req,
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id) {
        std::string text;
        if (auto err = parse_session_input_draft_request(req, text)) return std::move(*err);

        if (auto* entry = active_session_entry_for_workspace(ws, id)) {
            if (entry->sm) {
                entry->sm->set_input_draft(text);
                return session_input_draft_response(req, id, entry->sm->current_input_draft());
            }
        }

        auto maybe_meta = find_session_meta_for_workspace(ws, id);
        if (!maybe_meta.has_value()) {
            crow::response r(404);
            r.body = R"({"error":"session not found"})";
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        }

        SessionMeta meta = *maybe_meta;
        meta.input_draft = text;
        const auto project_dir = SessionStorage::get_project_dir(ws.cwd);
        SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);
        return session_input_draft_response(req, id, text);
    }

    crow::response clear_session_todos(
        const crow::request& req,
        const acecode::desktop::WorkspaceMeta& ws,
        const std::string& id) {
        if (auto* entry = active_session_entry_for_workspace(ws, id)) {
            if (entry->sm) {
                entry->sm->set_todos({});
                return session_todos_response(req, ws, id, entry->sm->current_todos());
            }
        }

        auto maybe_meta = find_session_meta_for_workspace(ws, id);
        if (!maybe_meta.has_value()) {
            crow::response r(404);
            r.body = R"({"error":"session not found"})";
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        }

        SessionMeta meta = *maybe_meta;
        meta.todos.clear();
        const auto project_dir = SessionStorage::get_project_dir(ws.cwd);
        SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);
        return session_todos_response(req, ws, id, meta.todos);
    }

    std::filesystem::path pinned_sessions_path_for_cwd(const std::string& cwd) const {
        return path_from_utf8(SessionStorage::get_project_dir(cwd)) /
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

        auto project_dir = SessionStorage::get_project_dir(ws.cwd);
        std::unordered_set<std::string> archived_ids;
        for (const auto& m : SessionStorage::list_sessions(project_dir)) {
            if (m.archived) archived_ids.insert(m.id);
        }

        if (deps.session_client) {
            for (const auto& s : deps.session_client->list_sessions()) {
                const bool same_workspace = s.workspace_hash == ws.hash ||
                    (s.workspace_hash.empty() && s.cwd == ws.cwd);
                if (archived_ids.count(s.id)) continue;
                if (same_workspace) add(s.id);
            }
        }

        for (const auto& m : SessionStorage::list_sessions(project_dir)) {
            if (m.archived) continue;
            add(m.id);
        }
        return out;
    }

    json pinned_sessions_to_json(const acecode::desktop::WorkspaceMeta& ws,
                                 const std::vector<std::string>& session_ids) const {
        return json{{"workspace_hash", ws.hash}, {"cwd", ws.cwd}, {"session_ids", session_ids}};
    }

    std::string attention_store_path_for_cwd(const std::string& cwd) const {
        return path_to_utf8(path_from_utf8(SessionStorage::get_project_dir(cwd)) /
                            "session_read_state.json");
    }

    void load_attention_workspace_locked(const std::string& workspace_hash,
                                         const std::string& cwd) const {
        if (workspace_hash.empty()) return;
        attention_workspace_cwds[workspace_hash] = cwd;
        if (loaded_attention_workspaces.count(workspace_hash)) return;
        loaded_attention_workspaces.insert(workspace_hash);

        std::ifstream in(path_from_utf8(attention_store_path_for_cwd(cwd)));
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

        const auto path = path_from_utf8(attention_store_path_for_cwd(cwd_it->second));
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        auto tmp = path;
        tmp += ".tmp";
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
            if (j.contains("name") && j["name"].is_string())
                opts.model_name = j["name"].get<std::string>();
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

    std::optional<SessionModelState> current_model_state_for_session(
        const std::string& session_id,
        const std::string& workspace_hash_hint = {}) const {
        if (session_id.empty() || !deps.session_registry) return std::nullopt;

        if (auto active = deps.session_registry->current_model_state(session_id)) {
            return active;
        }

        std::vector<acecode::desktop::WorkspaceMeta> workspaces;
        if (!workspace_hash_hint.empty()) {
            if (auto ws = resolve_workspace(workspace_hash_hint)) {
                workspaces.push_back(*ws);
            } else {
                return std::nullopt;
            }
        } else {
            workspaces.push_back(compatibility_workspace());
            if (deps.workspace_registry) {
                deps.workspace_registry->scan(projects_dir());
                for (const auto& ws : deps.workspace_registry->list()) {
                    if (std::none_of(workspaces.begin(), workspaces.end(),
                        [&](const auto& existing) { return existing.hash == ws.hash; })) {
                        workspaces.push_back(ws);
                    }
                }
            }
        }

        for (const auto& ws : workspaces) {
            auto project_dir = SessionStorage::get_project_dir(ws.cwd);
            auto candidates = SessionStorage::find_session_files(project_dir, session_id);
            if (candidates.empty()) continue;
            if (candidates.front().meta_path.empty()) continue;
            auto meta = SessionStorage::read_meta(candidates.front().meta_path);
            if (meta.id.empty()) continue;
            if (auto state = deps.session_registry->model_state_from_meta(meta)) {
                return state;
            }
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------
    // 路由注册
    // -----------------------------------------------------------------
    void register_routes() {
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
        register_websocket();
        register_static();
    }

    void register_workspaces() {
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
        CROW_ROUTE(app, "/api/files/blob").methods(crow::HTTPMethod::Options)
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
                    : path_from_utf8(cwd_q);

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

        // GET /api/files/blob?cwd=<abs>&path=<rel>
        CROW_ROUTE(app, "/api/files/blob").methods(crow::HTTPMethod::GET)
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

            auto mime = preview_image_mime(path_q);
            if (!mime.has_value()) {
                crow::response r(415);
                r.body = R"({"error":"unsupported file type"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto allowed_cwds = allowed_file_cwds();
            auto validated = validate_path_within(cwd_q, path_q, allowed_cwds);
            if (std::holds_alternative<FileError>(validated)) {
                return error_response(req, std::get<FileError>(validated));
            }
            auto abs_file = std::get<std::filesystem::path>(validated);

            std::error_code ec;
            if (!std::filesystem::exists(abs_file, ec) || ec) {
                return error_response(req, FileError{FileErrorKind::NotFound, 0, "file not found"});
            }
            if (std::filesystem::is_directory(abs_file, ec) || ec) {
                return error_response(req, FileError{FileErrorKind::NotFound, 0, "is a directory"});
            }
            auto sz = std::filesystem::file_size(abs_file, ec);
            if (ec) {
                return error_response(req, FileError{FileErrorKind::IoError, 0, ec.message()});
            }
            constexpr std::uint64_t kMaxImagePreviewBytes = 20ull * 1024 * 1024;
            if (sz > kMaxImagePreviewBytes) {
                return error_response(req, FileError{
                    FileErrorKind::TooLarge,
                    static_cast<std::uint64_t>(sz),
                    "file exceeds image preview cap",
                });
            }

            std::ifstream in(abs_file, std::ios::binary);
            if (!in) {
                return error_response(req, FileError{FileErrorKind::IoError, 0, "failed to open file"});
            }
            std::string body((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            if (!in.good() && !in.eof()) {
                return error_response(req, FileError{FileErrorKind::IoError, 0, "failed to read file"});
            }

            crow::response r(std::move(body));
            r.add_header("Content-Type", *mime);
            r.add_header("Cache-Control", "no-cache");
            r.add_header("X-Content-Type-Options", "nosniff");
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
            // desktop.notifications 透传给前端,desktopNotify.js 用它判抑制规则
            // (见 openspec/changes/add-desktop-attention-notifications)。
            // 浏览器直连 daemon 模式没有桌面壳桥,前端会自然 no-op,这里始终输出。
            if (deps.app_config) {
                const auto& n = deps.app_config->desktop.notifications;
                j["notifications"] = {
                    {"enabled", n.enabled},
                    {"on_question", n.on_question},
                    {"on_completion", n.on_completion},
                    {"suppress_when_focused", n.suppress_when_focused},
                };
            }
            crow::response r(j.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });
    }

    void register_usage() {
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
        CROW_ROUTE(app, "/api/sessions").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            LOG_INFO("[web] compatibility /api/sessions list for cwd=" + deps.cwd);
            auto arr = sessions_for_workspace(compatibility_workspace(), archived_query_requested(req));
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
            auto project_dir = SessionStorage::get_project_dir(ws.cwd);
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
            crow::response r(200);
            r.body = json{{"session_id", id}, {"id", id}, {"active", true}, {"workspace_hash", ws.hash}, {"cwd", ws.cwd}}.dump();
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

                auto project_dir = SessionStorage::get_project_dir(deps.cwd);
                auto candidates = SessionStorage::find_session_files(project_dir, id);
                if (!candidates.empty()) {
                    json msgs = json::array();
                    for (const auto& m : SessionStorage::load_messages(candidates.front().jsonl_path)) {
                        if (is_file_checkpoint_message(m)) continue;
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

            crow::response r(arr.dump());
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
            auto* entry = deps.session_registry->lookup(id);
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
            auto* entry = deps.session_registry->lookup(id);
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

            std::string text;
            json attachment_refs = json::array();
            json contexts = json::array();
            try {
                auto j = json::parse(req.body);
                if (j.contains("text") && j["text"].is_string()) {
                    text = j["text"].get<std::string>();
                }
                if (j.contains("attachments") && j["attachments"].is_array()) {
                    attachment_refs = j["attachments"];
                }
                if (j.contains("contexts") && j["contexts"].is_array()) {
                    contexts = j["contexts"];
                }
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (text.empty() && attachment_refs.empty() && contexts.empty()) {
                crow::response r(400);
                r.body = R"({"error":"text or attachment required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            // openspec/changes/expand-webui-skill-commands:Daemon 端 skill 命令展开。
            // 若 text 是 `/<skill-name> args` 形式且对应 session 所属 workspace 内
            // 有这个 skill,把 text 替换成轻量调用提示;UI 仍显示原文(走 metadata
            // .display_text)— LLM-prompt 与 UI-display 解耦。未命中透传。
            std::string original_text = text;
            bool expanded = false;
            SelectionPromptContext selection_context = build_selection_prompt_context(contexts);
            const bool selection_expanded = !selection_context.prompt.empty();
            if (attachment_refs.empty() && contexts.empty() &&
                deps.session_registry && deps.app_config) {
                if (auto* entry = deps.session_registry->lookup(id)) {
                    if (!entry->cwd.empty()) {
                        SkillRegistry tmp_skills;
                        initialize_skill_registry(tmp_skills, *deps.app_config, entry->cwd);
                        auto exp = web::try_expand_skill_command(text, tmp_skills);
                        if (exp.expanded) {
                            text = std::move(exp.text);
                            expanded = true;
                        }
                    }
                }
            }
            if (selection_expanded) {
                text = build_selection_augmented_prompt(selection_context, original_text);
            }

            UserInput input;
            input.text = text;
            if (expanded || selection_expanded) input.display_text = original_text;

            if (!attachment_refs.empty() || !contexts.empty()) {
                if (!deps.session_registry) {
                    crow::response r(503);
                    r.body = R"({"error":"session registry unavailable"})";
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                auto* entry = deps.session_registry->lookup(id);
                if (!entry) {
                    crow::response r(404);
                    r.body = R"({"error":"unknown session"})";
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }

                json parts = json::array();
                if (!text.empty()) {
                    parts.push_back(json{{"type", "text"}, {"text", text}});
                }

                json attachment_meta = json::array();
                const std::string project_dir = SessionStorage::get_project_dir(entry->cwd);
                for (const auto& ref : attachment_refs) {
                    std::string attachment_id;
                    if (ref.is_string()) {
                        attachment_id = ref.get<std::string>();
                    } else if (ref.is_object() && ref.contains("id") && ref["id"].is_string()) {
                        attachment_id = ref["id"].get<std::string>();
                    }
                    if (attachment_id.empty()) {
                        crow::response r(400);
                        r.body = R"({"error":"attachment id required"})";
                        r.add_header("Content-Type", "application/json");
                        return with_cors(req, std::move(r));
                    }

                    std::string error;
                    auto record = load_attachment(project_dir, id, attachment_id, &error);
                    if (!record.has_value()) {
                        crow::response r(404);
                        r.body = json{{"error", error.empty() ? "attachment not found" : error}}.dump();
                        r.add_header("Content-Type", "application/json");
                        return with_cors(req, std::move(r));
                    }

                    json meta = attachment_to_json(*record);
                    attachment_meta.push_back(meta);
                    // 按 MIME + 文件名重新分类(route-attachments-by-capability 1.7),
                    // 不直接信持久化 kind:SVG 等非视觉媒体归 file,避免误走图片 part。
                    const std::string part_kind =
                        attachment_kind_for_mime(record->mime_type, record->name);
                    parts.push_back(json{
                        {"type", part_kind == "image" ? "image" : "file"},
                        {"attachment", std::move(meta)},
                    });
                }

                json context_meta = json::array();
                for (const auto& ctx : contexts) {
                    if (!ctx.is_object()) continue;
                    if (json_string_field(ctx, "type") == "selection") {
                        auto meta = sanitized_selection_context_meta(ctx);
                        if (!meta.has_value()) continue;
                        context_meta.push_back(*meta);
                        parts.push_back(json{{"type", "selection_context"}, {"context", *meta}});
                    } else {
                        context_meta.push_back(ctx);
                        parts.push_back(json{{"type", "browser_context"}, {"context", ctx}});
                    }
                }

                input.content_parts = std::move(parts);
                input.metadata = json::object();
                if (!attachment_meta.empty()) {
                    input.metadata["attachments"] = std::move(attachment_meta);
                }
                if (!context_meta.empty()) {
                    input.metadata["contexts"] = std::move(context_meta);
                }
                if (selection_expanded) {
                    input.metadata["selection_context_expanded"] = true;
                    input.metadata["display_text"] = original_text;
                }
            }

            bool ok = deps.session_client->send_input(id, input);
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

            auto* entry = deps.session_registry->lookup(id);
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
            // 注入旧值再走校验。这样前端编辑表单不必每次让用户重输 api_key
            // (api_key 字段本身从不在 GET 响应里返回 — 前端没办法回填真值)。
            // 也覆盖 base_url 以防偶发未提交;model/provider/name 显式必填,
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

    void register_ui_preferences() {
        CROW_ROUTE(app, "/api/config/ui-preferences").methods(crow::HTTPMethod::Options)
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
            crow::response r(200);
            r.add_header("Content-Type", "application/json");
            r.body = ui_preferences_to_json(deps.app_config->web_ui).dump();
            return with_cors(req, std::move(r));
        });

        // GET /api/config/default-permission-mode: daemon default for new sessions.
        CROW_ROUTE(app, "/api/config/default-permission-mode").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return crow::response(503);
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

            auto result = acecode::upgrade::check_for_update(*deps.app_config,
                                                             ACECODE_VERSION);
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
                auto ids = parse_openai_model_ids(json::parse(response.text));
                crow::response r(json{{"models", ids}}.dump());
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

    void register_skills() {
        CROW_ROUTE(app, "/api/skills/root").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // GET /api/skills/root?workspace=<hash>: resolve effective skill directory
        CROW_ROUTE(app, "/api/skills/root").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::optional<acecode::desktop::WorkspaceMeta> ws;
            const char* workspace_q = req.url_params.get("workspace");
            if (workspace_q && *workspace_q) {
                ws = resolve_workspace(workspace_q);
                if (!ws.has_value()) {
                    crow::response r(404);
                    r.body = R"({"error":"workspace not found"})";
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
            } else {
                ws = compatibility_workspace();
            }

            auto selected = resolve_skill_root_for_cwd(ws->cwd);
            json body{
                {"path", path_to_utf8(selected.path)},
                {"source", selected.source},
                {"workspace_hash", ws->hash},
                {"cwd", ws->cwd},
            };
            crow::response r(body.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

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

    void register_commands() {
        // GET /api/commands?workspace=<hash>: webui-slash-commands 用,只读返回
        // builtin + skill 列表。`workspace` 参数让 handler 按目标 workspace cwd
        // 实时扫描该项目下的 .agent/skills、.acecode/skills,与 daemon 全局 skills
        // 合并 — desktop 多 workspace 共享一个 daemon,daemon 启动 cwd 固定,
        // 不带 workspace 时用户切到的 workspace 项目里的 skill 会看不到。
        CROW_ROUTE(app, "/api/commands").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            std::optional<std::string> workspace_cwd;
            const char* workspace_q = req.url_params.get("workspace");
            if (workspace_q && *workspace_q) {
                if (auto m = resolve_workspace(workspace_q)) {
                    if (!m->cwd.empty()) workspace_cwd = m->cwd;
                }
            }
            SkillRegistry empty_registry;
            const auto& registry = deps.skill_registry ? *deps.skill_registry : empty_registry;
            auto payload = build_commands_payload(registry, workspace_cwd, deps.app_config);
            crow::response r(payload.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
        CROW_ROUTE(app, "/api/commands").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
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
