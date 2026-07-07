// server_helpers.cpp — Shared helper method definitions for WebServer::Impl.
// This file contains auth/CORS, workspace, session serialization, attention,
// session draft/title/todo, and session-options helpers that are used by
// multiple route TUs.

#include "server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

// =====================================================================
// Free functions (formerly anonymous-namespace, now shared across route TUs)
// =====================================================================

std::uint64_t parse_seq(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoull(s); } catch (...) { return 0; }
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::optional<std::string> preview_blob_mime(const std::string& path) {
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
    if (ext == "pdf")  return "application/pdf";
    if (ext == "docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ext == "xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    if (ext == "xlsm") return "application/vnd.ms-excel.sheet.macroEnabled.12";
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

json custom_instructions_to_json(const CustomInstructionsConfig& cfg) {
    return json{{"text", cfg.text_snapshot()}};
}

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

json session_event_to_json(const SessionEvent& evt,
                            const std::string& session_id,
                            const std::string& workspace_hash,
                            const std::string& cwd) {
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

// =====================================================================
// Impl member helpers — Auth
// =====================================================================

AuthResult WebServer::Impl::auth_result_for_request(const crow::request& req,
                                                      const std::string& header_token,
                                                      const std::string& query_token) const {
    auto origin = req.get_header_value("Origin");
    if (!origin.empty() && !is_same_request_origin(req, origin)) {
        if (!is_loopback_origin(origin)) return AuthResult::BadToken;
        return check_explicit_token(deps.token, header_token, query_token);
    }
    return check_request_auth(req.remote_ip_address, deps.token,
                              header_token, query_token);
}

std::optional<crow::response> WebServer::Impl::require_auth(const crow::request& req) {
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

void WebServer::Impl::add_cors(const crow::request& req, crow::response& resp) {
    std::string origin = req.get_header_value("Origin");
    if (origin.empty() || !is_loopback_origin(origin)) return;
    resp.add_header("Access-Control-Allow-Origin", origin);
    resp.add_header("Vary", "Origin");
    resp.add_header("Access-Control-Allow-Credentials", "false");
    resp.add_header("Access-Control-Allow-Headers", "Content-Type, X-ACECode-Token");
    resp.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
}

crow::response WebServer::Impl::with_cors(const crow::request& req, crow::response resp) {
    add_cors(req, resp);
    return resp;
}

crow::response WebServer::Impl::cors_preflight(const crow::request& req) {
    crow::response r(204);
    add_cors(req, r);
    return r;
}

// =====================================================================
// Impl member helpers — Workspace
// =====================================================================

std::string WebServer::Impl::projects_dir() const {
    if (!deps.projects_dir.empty()) return deps.projects_dir;
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "projects");
}

std::string WebServer::Impl::no_workspace_cache_root() const {
    return deps.no_workspace_cache_root.empty()
        ? default_no_workspace_cache_root()
        : deps.no_workspace_cache_root;
}

acecode::desktop::WorkspaceMeta WebServer::Impl::compatibility_workspace() const {
    acecode::desktop::WorkspaceMeta m;
    m.cwd = deps.cwd;
    m.hash = compute_cwd_hash(deps.cwd);
    m.name = acecode::desktop::default_workspace_name(deps.cwd);
    return m;
}

std::optional<acecode::desktop::WorkspaceMeta> WebServer::Impl::resolve_workspace(const std::string& hash) const {
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

bool WebServer::Impl::archived_query_requested(const crow::request& req) const {
    auto raw = req.url_params.get("archived");
    if (!raw) return false;
    const std::string value = ascii_lower(raw);
    return value == "1" || value == "true" || value == "yes";
}

UsageLedgerQuery WebServer::Impl::usage_query_from_request(const crow::request& req) const {
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

std::vector<UsageLedgerScope> WebServer::Impl::usage_scopes_for_request(
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

std::vector<std::string> WebServer::Impl::allowed_file_cwds() const {
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

json WebServer::Impl::workspace_to_json(const acecode::desktop::WorkspaceMeta& m) const {
    json o;
    o["hash"] = m.hash;
    o["cwd"] = m.cwd;
    o["name"] = m.name;
    o["available"] = cwd_is_directory(m.cwd);
    return o;
}

// =====================================================================
// Impl member helpers — Session serialization
// =====================================================================

bool WebServer::Impl::token_usage_has_values(const TokenUsage& usage) {
    return usage.has_data ||
           usage.prompt_tokens != 0 ||
           usage.completion_tokens != 0 ||
           usage.total_tokens != 0 ||
           usage.cache_read_tokens != 0 ||
           usage.cache_write_tokens != 0 ||
           usage.reasoning_tokens != 0;
}

json WebServer::Impl::token_usage_to_json(const TokenUsage& usage) {
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

json WebServer::Impl::token_usage_or_null(const TokenUsage& usage) {
    return token_usage_has_values(usage) ? token_usage_to_json(usage) : json(nullptr);
}

bool WebServer::Impl::session_model_deleted(const std::string& model_name) const {
    if (model_name.empty() || model_name.rfind("(session:", 0) == 0 || !deps.app_config) {
        return false;
    }
    std::lock_guard<std::mutex> config_lock(app_config_mu);
    for (const auto& entry : deps.app_config->saved_models) {
        if (entry.name == model_name) return false;
    }
    return true;
}

json WebServer::Impl::session_info_to_json(const SessionInfo& s, const SessionMeta* m) const {
    json o;
    const std::string model_name =
        !s.model_name.empty() ? s.model_name : (m ? m->model_preset : "");
    const bool model_deleted = s.model_deleted || session_model_deleted(model_name);
    const bool no_workspace = s.no_workspace || (m && m->no_workspace);
    const std::string workspace_hash = no_workspace
        ? std::string{}
        : (!s.workspace_hash.empty() ? s.workspace_hash : (m ? compute_cwd_hash(m->cwd) : ""));
    const std::string cwd = no_workspace ? std::string{} : (!s.cwd.empty() ? s.cwd : (m ? m->cwd : ""));
    o["id"]            = s.id;
    o["active"]        = true;
    o["status"]        = s.busy ? "running" : "idle";
    o["workspace_hash"] = workspace_hash;
    o["cwd"]           = cwd;
    o["no_workspace"]  = no_workspace;
    o["title"]         = !s.title.empty() ? s.title : (m ? m->title : "");
    o["title_source"]  = !s.title_source.empty() ? s.title_source : (m ? m->title_source : "");
    o["summary"]       = !s.summary.empty() ? s.summary : (m ? m->summary : "");
    o["created_at"]    = !s.created_at.empty() ? s.created_at : (m ? m->created_at : "");
    o["updated_at"]    = !s.updated_at.empty() ? s.updated_at : (m ? m->updated_at : "");
    o["provider"]      = model_deleted ? "" : (!s.provider.empty() ? s.provider : (m ? m->provider : ""));
    o["model"]         = model_deleted ? "" : (!s.model.empty() ? s.model : (m ? m->model : ""));
    o["model_name"]    = model_name;
    o["model_preset"]  = o["model_name"];
    o["context_window"] = s.context_window;
    o["deleted"]       = model_deleted;
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
    o["parent_session_id"] = !s.parent_session_id.empty()
        ? s.parent_session_id
        : (m ? m->parent_session_id : std::string{});
    append_attention_fields(o, s.id, workspace_hash, cwd, s.busy);
    return o;
}

json WebServer::Impl::session_meta_to_json(const SessionMeta& m, const std::string& workspace_hash) const {
    json o;
    const bool model_deleted = session_model_deleted(m.model_preset);
    const std::string effective_workspace_hash = m.no_workspace ? std::string{} : workspace_hash;
    const std::string effective_cwd = m.no_workspace ? std::string{} : m.cwd;
    o["id"]             = m.id;
    o["active"]         = false;
    o["status"]         = "idle";
    o["workspace_hash"] = effective_workspace_hash;
    o["cwd"]            = effective_cwd;
    o["no_workspace"]   = m.no_workspace;
    o["title"]          = m.title;
    o["title_source"]   = m.title_source;
    o["summary"]        = m.summary;
    o["created_at"]     = m.created_at;
    o["updated_at"]     = m.updated_at;
    o["provider"]       = model_deleted ? "" : m.provider;
    o["model"]          = model_deleted ? "" : m.model;
    o["model_name"]     = m.model_preset;
    o["model_preset"]   = m.model_preset;
    o["deleted"]        = model_deleted;
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
    o["parent_session_id"] = m.parent_session_id;
    append_attention_fields(o, m.id, effective_workspace_hash, effective_cwd, false);
    return o;
}

void WebServer::Impl::append_session_runtime_snapshot(json& wrapper,
                                                        const std::string& session_id) const {
    if (session_id.empty()) return;
    SessionMeta meta;
    bool have_meta = false;
    if (deps.session_registry) {
        if (auto entry = deps.session_registry->acquire(session_id)) {
            if (entry->no_workspace) {
                wrapper["no_workspace"] = true;
                wrapper["workspace_hash"] = "";
                wrapper["cwd"] = "";
            }
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
        if (!have_meta) {
            auto no_workspace_meta = find_no_workspace_session_meta(session_id);
            if (no_workspace_meta.has_value()) {
                meta = *no_workspace_meta;
                have_meta = true;
            }
        }
    }
    if (have_meta) {
        if (meta.no_workspace) {
            wrapper["no_workspace"] = true;
            wrapper["workspace_hash"] = "";
            wrapper["cwd"] = "";
        }
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
    auto entry = deps.session_registry->acquire(session_id);
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

json WebServer::Impl::sessions_for_workspace(const acecode::desktop::WorkspaceMeta& ws,
                                               bool archived_only,
                                               bool include_no_workspace,
                                               const std::string& parent_filter) const {
    std::vector<SessionInfo> active;
    if (deps.session_client) active = deps.session_client->list_sessions();

    auto project_dir = SessionStorage::get_project_dir(ws.cwd);
    auto disk = SessionStorage::list_sessions(project_dir);
    if (include_no_workspace) {
        auto no_workspace_disk = no_workspace_disk_sessions();
        disk.insert(disk.end(), no_workspace_disk.begin(), no_workspace_disk.end());
        std::sort(disk.begin(), disk.end(),
                  [](const SessionMeta& a, const SessionMeta& b) {
                      return a.updated_at > b.updated_at;
                  });
    }

    std::unordered_map<std::string, SessionMeta> disk_by_id;
    for (const auto& m : disk) {
        disk_by_id[m.id] = m;
    }

    // 子会话过滤:常规列表(parent_filter 空)隐藏所有后台任务子会话;
    // 后台任务查询(parent_filter 非空)只保留指定父会话的子会话。
    const auto parent_mismatch = [&](const std::string& parent_id) {
        if (parent_filter.empty()) return !parent_id.empty();
        return parent_id != parent_filter;
    };

    std::unordered_set<std::string> seen;
    json arr = json::array();
    for (const auto& s : active) {
        if (parent_mismatch(s.parent_session_id)) continue;
        if (parent_filter.empty()) {
            // 常规列表按 workspace 归属过滤;后台任务查询跳过该过滤
            // (子会话跟随父会话归属,包括 no-workspace 父的子会话)。
            if (s.no_workspace) {
                if (!include_no_workspace) continue;
            } else if (s.workspace_hash != ws.hash) {
                continue;
            }
        }
        seen.insert(s.id);
        auto meta_it = disk_by_id.find(s.id);
        const SessionMeta* m = meta_it == disk_by_id.end() ? nullptr : &meta_it->second;
        const bool archived = m ? m->archived : false;
        if (archived != archived_only) continue;
        arr.push_back(session_info_to_json(s, m));
    }
    for (const auto& m : disk) {
        if (seen.count(m.id)) continue;
        if (parent_mismatch(m.parent_session_id)) continue;
        if (m.archived != archived_only) continue;
        if (m.no_workspace && !include_no_workspace) continue;
        arr.push_back(session_meta_to_json(m, m.no_workspace ? std::string{} : ws.hash));
    }
    return arr;
}

std::vector<SessionMeta> WebServer::Impl::no_workspace_disk_sessions() const {
    std::vector<SessionMeta> out;
    for (const auto& cwd : list_no_workspace_session_cwds(no_workspace_cache_root())) {
        for (const auto& meta : SessionStorage::list_sessions(SessionStorage::get_project_dir(cwd))) {
            if (meta.no_workspace) out.push_back(meta);
        }
    }
    return out;
}

std::optional<SessionMeta>
WebServer::Impl::find_no_workspace_session_meta(const std::string& id) const {
    if (id.empty()) return std::nullopt;
    const auto direct_cwd = no_workspace_session_cwd(id, no_workspace_cache_root());
    auto direct_meta = SessionStorage::read_meta(
        SessionStorage::meta_path(SessionStorage::get_project_dir(direct_cwd), id));
    if (!direct_meta.id.empty() && direct_meta.no_workspace) return direct_meta;

    for (const auto& cwd : list_no_workspace_session_cwds(no_workspace_cache_root())) {
        if (cwd == direct_cwd) continue;
        auto meta = SessionStorage::read_meta(
            SessionStorage::meta_path(SessionStorage::get_project_dir(cwd), id));
        if (!meta.id.empty() && meta.no_workspace) return meta;
    }
    return std::nullopt;
}

bool WebServer::Impl::session_entry_matches_workspace(const SessionEntry& entry,
                                                        const acecode::desktop::WorkspaceMeta& ws) const {
    if (entry.no_workspace) return false;
    if (!entry.workspace_hash.empty()) return entry.workspace_hash == ws.hash;
    return entry.cwd == ws.cwd;
}

std::optional<SessionMeta> WebServer::Impl::find_session_meta_for_workspace(
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

    if (auto meta = find_no_workspace_session_meta(id)) {
        return meta;
    }

    if (deps.session_registry) {
        if (auto entry = deps.session_registry->acquire(id)) {
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
            meta.no_workspace = entry->no_workspace;
            meta.parent_session_id = entry->parent_session_id;
            if (entry->sm) {
                meta.title = entry->sm->current_title();
                meta.title_source = entry->sm->current_title_source();
                meta.input_draft = entry->sm->current_input_draft();
            }
            return meta;
        }
    }

    return std::nullopt;
}

// =====================================================================
// Impl member helpers — Session draft/title/todo/response
// =====================================================================

crow::response WebServer::Impl::set_session_archive_state(
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
        if (auto entry = deps.session_registry->acquire(id)) {
            if (session_entry_matches_workspace(*entry, ws) && entry->sm) {
                entry->sm->set_session_archived(false);
                const auto reread = find_session_meta_for_workspace(ws, id);
                if (reread.has_value()) meta = *reread;
            }
        }
    }

    meta.archived = archived;
    const auto project_dir = SessionStorage::get_project_dir(
        meta.no_workspace && !meta.cwd.empty() ? meta.cwd : ws.cwd);
    SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);

    crow::response r(session_meta_to_json(meta, ws.hash).dump());
    r.add_header("Content-Type", "application/json");
    return with_cors(req, std::move(r));
}

crow::response WebServer::Impl::session_input_draft_response(
    const crow::request& req,
    const std::string& id,
    const std::string& text) {
    crow::response r(json{{"session_id", id}, {"id", id}, {"text", text}}.dump());
    r.add_header("Content-Type", "application/json");
    return with_cors(req, std::move(r));
}

crow::response WebServer::Impl::session_todos_response(
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

std::optional<crow::response> WebServer::Impl::parse_session_input_draft_request(
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

std::shared_ptr<SessionEntry> WebServer::Impl::active_session_entry_for_workspace(
    const acecode::desktop::WorkspaceMeta& ws,
    const std::string& id) const {
    if (!deps.session_registry) return nullptr;
    auto entry = deps.session_registry->acquire(id);
    if (!entry || !session_entry_matches_workspace(*entry, ws)) return nullptr;
    return entry;
}

void WebServer::Impl::emit_session_title_update(SessionEntry& entry) const {
    if (!entry.loop || !entry.sm) return;
    entry.loop->events().emit(SessionEventKind::SessionUpdated, json{
        {"session_id", entry.id},
        {"workspace_hash", entry.workspace_hash},
        {"cwd", entry.cwd},
        {"title", entry.sm->current_title()},
        {"title_source", entry.sm->current_title_source()},
    });
}

std::optional<crow::response> WebServer::Impl::parse_session_title_request(
    const crow::request& req,
    std::string& title) {
    try {
        auto j = json::parse(req.body);
        if (!j.contains("title") || !j["title"].is_string()) {
            crow::response r(400);
            r.body = R"({"error":"title required"})";
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        }
        title = j["title"].get<std::string>();
    } catch (const std::exception& e) {
        crow::response r(400);
        r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    }

    while (!title.empty() && std::isspace(static_cast<unsigned char>(title.front()))) {
        title.erase(title.begin());
    }
    while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back()))) {
        title.pop_back();
    }
    std::string err;
    if (!sanitize_title(title, err)) {
        crow::response r(400);
        r.body = json{{"error", "invalid title"}, {"message", err}}.dump();
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    }
    return std::nullopt;
}

crow::response WebServer::Impl::set_session_title_response(
    const crow::request& req,
    const acecode::desktop::WorkspaceMeta& ws,
    const std::string& id) {
    std::string title;
    if (auto err = parse_session_title_request(req, title)) return std::move(*err);

    if (auto entry = active_session_entry_for_workspace(ws, id)) {
        if (entry->sm) {
            entry->sm->ensure_active_session_id();
            entry->sm->set_session_title(title);
            emit_session_title_update(*entry);
            if (auto meta = find_session_meta_for_workspace(ws, id)) {
                crow::response r(session_info_to_json(
                    SessionInfo{
                        id,
                        entry->cwd,
                        entry->workspace_hash,
                        meta->created_at,
                        meta->updated_at,
                        meta->summary,
                        entry->model_state.name,
                        entry->provider,
                        entry->model,
                        entry->model_state.context_window,
                        entry->model_state.deleted,
                        entry->sm->current_title(),
                        entry->sm->current_title_source(),
                        meta->message_count,
                        entry->sm->current_turn_count(),
                        entry->sm->current_permission_mode(),
                        entry->sm->current_last_token_usage(),
                        entry->sm->current_session_token_usage(),
                        true,
                        entry->loop ? entry->loop->is_busy() : false,
                    },
                    &*meta).dump());
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
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
    meta.title = title;
    meta.title_source = title.empty() ? std::string{} : "user";
    const auto project_dir = SessionStorage::get_project_dir(
        meta.no_workspace && !meta.cwd.empty() ? meta.cwd : ws.cwd);
    SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);

    crow::response r(session_meta_to_json(meta, ws.hash).dump());
    r.add_header("Content-Type", "application/json");
    return with_cors(req, std::move(r));
}

crow::response WebServer::Impl::get_session_input_draft(
    const crow::request& req,
    const acecode::desktop::WorkspaceMeta& ws,
    const std::string& id) {
    if (auto entry = active_session_entry_for_workspace(ws, id)) {
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

crow::response WebServer::Impl::set_session_input_draft(
    const crow::request& req,
    const acecode::desktop::WorkspaceMeta& ws,
    const std::string& id) {
    std::string text;
    if (auto err = parse_session_input_draft_request(req, text)) return std::move(*err);

    if (auto entry = active_session_entry_for_workspace(ws, id)) {
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
    const auto project_dir = SessionStorage::get_project_dir(
        meta.no_workspace && !meta.cwd.empty() ? meta.cwd : ws.cwd);
    SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);
    return session_input_draft_response(req, id, text);
}

crow::response WebServer::Impl::clear_session_todos(
    const crow::request& req,
    const acecode::desktop::WorkspaceMeta& ws,
    const std::string& id) {
    if (auto entry = active_session_entry_for_workspace(ws, id)) {
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
    const auto project_dir = SessionStorage::get_project_dir(
        meta.no_workspace && !meta.cwd.empty() ? meta.cwd : ws.cwd);
    SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);
    return session_todos_response(req, ws, id, meta.todos);
}

std::filesystem::path WebServer::Impl::pinned_sessions_path_for_cwd(const std::string& cwd) const {
    return path_from_utf8(SessionStorage::get_project_dir(cwd)) /
           "pinned_sessions.json";
}

std::filesystem::path WebServer::Impl::pinned_session_order_path() const {
    return path_from_utf8(projects_dir()) / "pinned_sessions_order.json";
}

std::vector<std::string> WebServer::Impl::session_ids_for_workspace(
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

json WebServer::Impl::pinned_sessions_to_json(const acecode::desktop::WorkspaceMeta& ws,
                                                 const std::vector<std::string>& session_ids) const {
    return json{{"workspace_hash", ws.hash}, {"cwd", ws.cwd}, {"session_ids", session_ids}};
}

std::vector<PinnedSessionOrderItem> WebServer::Impl::available_pinned_session_order_items() const {
    std::vector<PinnedSessionOrderItem> out;
    std::unordered_set<std::string> seen;

    auto add_workspace = [&](const acecode::desktop::WorkspaceMeta& ws) {
        if (ws.hash.empty()) return;
        const auto path = pinned_sessions_path_for_cwd(ws.cwd);
        auto state = read_pinned_sessions_state(path);
        const auto pruned = prune_pinned_session_ids(
            state.session_ids, session_ids_for_workspace(ws));
        if (pruned != state.session_ids) {
            std::string ignored;
            write_pinned_sessions_state(path, PinnedSessionsState{pruned}, &ignored);
        }
        for (const auto& id : pruned) {
            if (id.empty()) continue;
            const auto key = ws.hash + '\0' + id;
            if (seen.count(key)) continue;
            seen.insert(key);
            out.push_back(PinnedSessionOrderItem{ws.hash, id});
        }
    };

    if (deps.workspace_registry) {
        deps.workspace_registry->scan(projects_dir());
        for (const auto& ws : deps.workspace_registry->list()) {
            add_workspace(ws);
        }
    } else {
        add_workspace(compatibility_workspace());
    }
    return out;
}

json WebServer::Impl::pinned_session_order_to_json(
    const std::vector<PinnedSessionOrderItem>& items) const {
    json arr = json::array();
    for (const auto& item : items) {
        arr.push_back(json{
            {"workspace_hash", item.workspace_hash},
            {"session_id", item.session_id},
        });
    }
    return json{{"items", arr}};
}

// =====================================================================
// Impl member helpers — Attention state
// =====================================================================

std::string WebServer::Impl::attention_store_path_for_cwd(const std::string& cwd) const {
    return path_to_utf8(path_from_utf8(SessionStorage::get_project_dir(cwd)) /
                        "session_read_state.json");
}

void WebServer::Impl::load_attention_workspace_locked(const std::string& workspace_hash,
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

void WebServer::Impl::save_attention_workspace_locked(const std::string& workspace_hash) const {
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

SessionAttentionRecord WebServer::Impl::attention_record_for_session(
    const std::string& workspace_hash,
    const std::string& cwd,
    const std::string& session_id,
    bool busy) const {
    std::lock_guard<std::mutex> lk(attention_mu);
    load_attention_workspace_locked(workspace_hash, cwd);
    auto record = attention_by_workspace[workspace_hash][session_id];
    record.busy = busy;
    return record;
}

json WebServer::Impl::attention_payload_for_record(
    const std::string& session_id,
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

void WebServer::Impl::append_attention_fields(json& o,
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

std::optional<acecode::desktop::WorkspaceMeta> WebServer::Impl::resolve_session_workspace(
    const std::string& session_id,
    const std::string& workspace_hash_hint) const {
    if (!session_id.empty() && deps.session_registry) {
        if (auto entry = deps.session_registry->acquire(session_id)) {
            acecode::desktop::WorkspaceMeta ws;
            if (entry->no_workspace) {
                ws.cwd = entry->cwd;
                ws.name = "";
                return ws;
            }
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

void WebServer::Impl::broadcast_session_status(const json& payload) {
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

void WebServer::Impl::track_subagent(const std::string& child_id) {
    if (child_id.empty() || !deps.session_client || !deps.session_registry) return;
    {
        std::lock_guard<std::mutex> lk(tracked_subagents_mu);
        if (tracked_subagent_subscriptions.find(child_id) !=
            tracked_subagent_subscriptions.end()) {
            return;
        }
    }
    std::string ws_hash;
    std::string cwd;
    if (auto e = deps.session_registry->acquire(child_id)) {
        ws_hash = e->workspace_hash;
        cwd = e->cwd;
    }
    // since=0:只关心从现在起的实时事件(子会话刚 spawn,还没历史)。回调仅把
    // 事件喂给 attention/status 广播,不向任何 WS 连接直发(那由各自的订阅负责)。
    auto tracker = std::weak_ptr<SubagentTrackerState>(subagent_tracker_state);
    auto sub = deps.session_client->subscribe(child_id,
        [tracker, child_id, ws_hash, cwd](const SessionEvent& evt) {
            auto state = tracker.lock();
            if (!state) return;
            std::lock_guard<std::mutex> lk(state->mu);
            if (!state->impl) return;
            state->impl->note_session_event_for_attention(child_id, ws_hash, cwd, evt);
        },
        /*since_seq=*/0);
    if (sub == 0) return;
    bool duplicate = false;
    {
        std::lock_guard<std::mutex> lk(tracked_subagents_mu);
        auto [_, inserted] = tracked_subagent_subscriptions.emplace(child_id, sub);
        duplicate = !inserted;
    }
    if (duplicate) {
        deps.session_client->unsubscribe(child_id, sub);
        return;
    }
    LOG_INFO("[web] track_subagent " + child_id + " ws=" + ws_hash);
}

void WebServer::Impl::note_session_event_for_attention(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& cwd,
    const SessionEvent& evt) {
    if (session_id.empty()) return;
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

json WebServer::Impl::mark_session_read_status(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& cwd,
    std::uint64_t cursor) {
    json payload;
    bool changed = false;
    bool current_busy = false;
    if (deps.session_registry) {
        if (auto entry = deps.session_registry->acquire(session_id)) {
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

void WebServer::Impl::send_status_snapshot(crow::websocket::connection& conn,
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

// =====================================================================
// Impl member helpers — Session options
// =====================================================================

void WebServer::Impl::refresh_default_session_preferences_for_new_session() {
    std::lock_guard<std::mutex> config_lock(app_config_mu);
    refresh_default_session_preferences_for_new_session_locked();
}

void WebServer::Impl::refresh_default_session_preferences_for_new_session_locked() {
    if (!deps.app_config) return;
    std::string err;
    if (!refresh_default_session_preferences_from_config(
            *deps.app_config, deps.config_path, &err)) {
        LOG_WARN("[web] failed to refresh default session preferences: " + err);
        return;
    }
    if (deps.session_registry) {
        auto parsed = parse_permission_mode_name(deps.app_config->default_permission_mode);
        deps.session_registry->set_default_permission_mode(
            parsed.value_or(PermissionMode::Default));
    }
}

std::optional<crow::response> WebServer::Impl::parse_session_options(
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
        std::string permission_mode_name;
        if (j.contains("permission_mode") && j["permission_mode"].is_string()) {
            permission_mode_name = j["permission_mode"].get<std::string>();
        } else if (j.contains("permissionMode") && j["permissionMode"].is_string()) {
            permission_mode_name = j["permissionMode"].get<std::string>();
        }
        if (!permission_mode_name.empty()) {
            auto parsed = parse_permission_mode_name(permission_mode_name);
            if (!parsed.has_value()) {
                crow::response r(400);
                r.body = json{{"error", "INVALID_PERMISSION_MODE"},
                              {"message", "invalid permission mode"}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            opts.permission_mode = PermissionManager::mode_name(*parsed);
        }
        if (j.contains("initial_user_message") && j["initial_user_message"].is_string())
            opts.initial_user_message = j["initial_user_message"].get<std::string>();
        if (j.contains("auto_start") && j["auto_start"].is_boolean())
            opts.auto_start = j["auto_start"].get<bool>();
        const bool no_workspace =
            (j.contains("no_workspace") && j["no_workspace"].is_boolean() && j["no_workspace"].get<bool>()) ||
            (j.contains("noWorkspace") && j["noWorkspace"].is_boolean() && j["noWorkspace"].get<bool>());
        if (no_workspace) {
            opts.no_workspace = true;
            opts.workspace_hash.clear();
        }
    } catch (const std::exception& e) {
        crow::response r(400);
        r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    }
    return std::nullopt;
}

std::optional<SessionModelState> WebServer::Impl::current_model_state_for_session(
    const std::string& session_id,
    const std::string& workspace_hash_hint) const {
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
    if (auto meta = find_no_workspace_session_meta(session_id)) {
        if (auto state = deps.session_registry->model_state_from_meta(*meta)) {
            return state;
        }
    }
    return std::nullopt;
}

} // namespace acecode::web
