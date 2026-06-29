#include "opencode_import.hpp"

#include "session_serializer.hpp"
#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/encoding.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace acecode {

namespace {

using nlohmann::json;

std::string ascii_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_generic_path_text(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.size() > 1 && text.back() == '/') text.pop_back();
#ifdef _WIN32
    text = ascii_lower_copy(std::move(text));
#endif
    return text;
}

std::string normalized_path_for_match(const std::string& raw) {
    if (raw.empty()) return {};
    std::error_code ec;
    fs::path p = path_from_utf8(raw);
    fs::path normalized = fs::weakly_canonical(p, ec);
    if (ec || normalized.empty()) {
        ec.clear();
        normalized = fs::absolute(p, ec);
        if (ec || normalized.empty()) normalized = p;
        normalized = normalized.lexically_normal();
    }
    return normalize_generic_path_text(path_to_utf8_generic(normalized));
}

bool path_matches_workspace(const std::string& candidate, const std::string& workspace) {
    const std::string c = normalized_path_for_match(candidate);
    const std::string w = normalized_path_for_match(workspace);
    if (c.empty() || w.empty()) return false;
    if (c == w) return true;
    return c.size() > w.size() &&
           c.compare(0, w.size(), w) == 0 &&
           c[w.size()] == '/';
}

std::string source_identity_for_path(const std::string& path) {
    if (path == ":memory:") return path;
    std::error_code ec;
    fs::path p = path_from_utf8(path);
    fs::path normalized = fs::weakly_canonical(p, ec);
    if (ec || normalized.empty()) {
        ec.clear();
        normalized = fs::absolute(p, ec);
        if (ec || normalized.empty()) normalized = p;
        normalized = normalized.lexically_normal();
    }
    return normalize_generic_path_text(path_to_utf8_generic(normalized));
}

std::string getenv_or_empty(const char* name) {
    return getenv_utf8(name);
}

std::vector<fs::path> opencode_data_dir_candidates() {
    std::vector<fs::path> dirs;
    auto add = [&](const std::string& value) {
        if (!value.empty()) dirs.push_back(path_from_utf8(value) / "opencode");
    };

    add(getenv_or_empty("XDG_DATA_HOME"));

#ifdef _WIN32
    if (const std::string home = getenv_or_empty("USERPROFILE"); !home.empty()) {
        dirs.push_back(path_from_utf8(home) / ".local" / "share" / "opencode");
    }
    add(getenv_or_empty("LOCALAPPDATA"));
    add(getenv_or_empty("APPDATA"));
#else
    if (const std::string home = getenv_or_empty("HOME"); !home.empty()) {
        dirs.push_back(path_from_utf8(home) / ".local" / "share" / "opencode");
    }
#endif

    return dirs;
}

void add_unique_path(std::vector<std::string>& out,
                     std::unordered_set<std::string>& seen,
                     const fs::path& path,
                     bool require_exists) {
    std::error_code ec;
    if (require_exists && !fs::is_regular_file(path, ec)) return;
    const std::string text = path_to_utf8(path);
    const std::string key = source_identity_for_path(text);
    if (seen.insert(key).second) out.push_back(text);
}

class SqliteDb {
public:
    explicit SqliteDb(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown sqlite error";
            if (db_) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error("open opencode database failed: " + msg);
        }
        exec("PRAGMA query_only=ON;");
        exec("PRAGMA busy_timeout=1000;");
    }

    ~SqliteDb() {
        if (db_) sqlite3_close(db_);
    }

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    sqlite3* get() const { return db_; }

private:
    void exec(const char* sql) {
        char* raw_error = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &raw_error);
        if (rc == SQLITE_OK) return;
        std::string msg = raw_error ? raw_error : sqlite3_errmsg(db_);
        sqlite3_free(raw_error);
        throw std::runtime_error("configure opencode database failed: " + msg);
    }

    sqlite3* db_ = nullptr;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("prepare opencode query failed: ") +
                                     sqlite3_errmsg(db_));
        }
    }

    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind_text(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            throw std::runtime_error(std::string("bind opencode query failed: ") +
                                     sqlite3_errmsg(db_));
        }
    }

    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(std::string("read opencode query failed: ") +
                                 sqlite3_errmsg(db_));
    }

    std::string text(int column) const {
        const unsigned char* raw = sqlite3_column_text(stmt_, column);
        return raw ? reinterpret_cast<const char*>(raw) : std::string{};
    }

    std::int64_t i64(int column) const {
        if (sqlite3_column_type(stmt_, column) == SQLITE_NULL) return 0;
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt_, column));
    }

    int integer(int column) const {
        return static_cast<int>(sqlite3_column_int(stmt_, column));
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

std::set<std::string> table_columns(sqlite3* db, const std::string& table) {
    Statement stmt(db, ("PRAGMA table_info(" + table + ");").c_str());
    std::set<std::string> columns;
    while (stmt.step()) {
        columns.insert(stmt.text(1));
    }
    return columns;
}

void require_columns(sqlite3* db,
                     const std::string& table,
                     const std::vector<std::string>& columns) {
    const auto found = table_columns(db, table);
    if (found.empty()) {
        throw std::runtime_error("unsupported opencode schema: missing table " + table);
    }
    for (const auto& column : columns) {
        if (!found.count(column)) {
            throw std::runtime_error("unsupported opencode schema: missing " +
                                     table + "." + column);
        }
    }
}

void validate_opencode_schema(sqlite3* db) {
    require_columns(db, "project", {"id", "worktree"});
    require_columns(db, "session", {
        "id", "project_id", "directory", "title", "time_created", "time_updated", "model"
    });
    require_columns(db, "message", {"id", "session_id", "time_created", "time_updated", "data"});
    require_columns(db, "part", {"id", "message_id", "session_id", "time_created", "time_updated", "data"});
}

json parse_json_or_null(const std::string& text) {
    if (text.empty()) return nullptr;
    try {
        return json::parse(text);
    } catch (...) {
        return nullptr;
    }
}

std::string json_string_any(const json& object,
                            const std::vector<const char*>& keys,
                            const std::string& fallback = {}) {
    if (!object.is_object()) return fallback;
    for (const char* key : keys) {
        auto it = object.find(key);
        if (it != object.end() && it->is_string()) return it->get<std::string>();
    }
    return fallback;
}

void extract_model_fields(const std::string& raw_model,
                          std::string& provider,
                          std::string& model) {
    const json j = parse_json_or_null(raw_model);
    if (j.is_object()) {
        provider = json_string_any(j, {"providerID", "provider_id", "provider"}, provider);
        model = json_string_any(j, {"id", "modelID", "model_id", "model"}, model);
    } else if (!raw_model.empty()) {
        model = raw_model;
    }
}

std::filesystem::path manifest_path(const std::string& project_dir) {
    return path_from_utf8(project_dir) / "opencode_imports.json";
}

json read_manifest(const std::string& project_dir) {
    std::ifstream in(manifest_path(project_dir), std::ios::binary);
    if (!in.is_open()) return json{{"version", 1}, {"imports", json::array()}};
    try {
        json j = json::parse(in);
        if (!j.is_object()) return json{{"version", 1}, {"imports", json::array()}};
        if (!j.contains("imports") || !j["imports"].is_array()) j["imports"] = json::array();
        if (!j.contains("version")) j["version"] = 1;
        return j;
    } catch (...) {
        return json{{"version", 1}, {"imports", json::array()}};
    }
}

bool acecode_target_exists(const std::string& project_dir, const std::string& session_id) {
    if (session_id.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(path_from_utf8(SessionStorage::session_path(project_dir, session_id)), ec) &&
           fs::is_regular_file(path_from_utf8(SessionStorage::meta_path(project_dir, session_id)), ec);
}

bool imported_target_still_exists(const json& manifest,
                                  const std::string& project_dir,
                                  const std::string& source_identity,
                                  const std::string& opencode_session_id) {
    const auto& imports = manifest.value("imports", json::array());
    if (!imports.is_array()) return false;
    for (const auto& entry : imports) {
        if (!entry.is_object()) continue;
        if (entry.value("source_identity", std::string{}) != source_identity) continue;
        if (entry.value("opencode_session_id", std::string{}) != opencode_session_id) continue;
        if (acecode_target_exists(project_dir, entry.value("acecode_session_id", std::string{}))) {
            return true;
        }
    }
    return false;
}

void append_manifest_entry(const std::string& project_dir,
                           const OpencodeSourceSession& source,
                           const std::string& acecode_session_id) {
    json manifest = read_manifest(project_dir);
    manifest["version"] = 1;
    if (!manifest.contains("imports") || !manifest["imports"].is_array()) {
        manifest["imports"] = json::array();
    }
    manifest["imports"].push_back(json{
        {"source_database", source.source_database},
        {"source_identity", source.source_identity},
        {"opencode_session_id", source.opencode_session_id},
        {"acecode_session_id", acecode_session_id},
        {"imported_at", SessionStorage::now_iso8601()},
    });
    atomic_write_file(path_to_utf8(manifest_path(project_dir)), manifest.dump(2) + "\n");
}

std::string iso8601_from_unix_ms(std::int64_t ms) {
    if (ms <= 0) return SessionStorage::now_iso8601();
    auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::vector<OpencodeSourceSession> query_source_sessions(sqlite3* db,
                                                         const std::string& database_path,
                                                         const std::string& cwd,
                                                         const std::string& project_dir) {
    validate_opencode_schema(db);
    const std::string source_identity = source_identity_for_path(database_path);
    const json manifest = read_manifest(project_dir);

    Statement stmt(db,
        "SELECT s.id, COALESCE(NULLIF(s.directory, ''), p.worktree, ''), "
        "COALESCE(s.title, ''), COALESCE(s.model, ''), "
        "s.time_created, s.time_updated, "
        "(SELECT COUNT(*) FROM message m WHERE m.session_id = s.id), "
        "(SELECT COUNT(*) FROM part pr WHERE pr.session_id = s.id) "
        "FROM session s "
        "LEFT JOIN project p ON p.id = s.project_id "
        "ORDER BY s.time_updated DESC, s.id DESC;");

    std::vector<OpencodeSourceSession> sessions;
    while (stmt.step()) {
        OpencodeSourceSession session;
        session.source_database = database_path;
        session.source_identity = source_identity;
        session.opencode_session_id = stmt.text(0);
        session.directory = stmt.text(1);
        session.title = stmt.text(2);
        extract_model_fields(stmt.text(3), session.provider, session.model);
        session.time_created_ms = stmt.i64(4);
        session.time_updated_ms = stmt.i64(5);
        session.message_count = stmt.integer(6);
        session.part_count = stmt.integer(7);
        if (session.opencode_session_id.empty()) continue;
        if (session.message_count <= 0 && session.part_count <= 0) continue;
        if (!path_matches_workspace(session.directory, cwd)) continue;
        if (imported_target_still_exists(manifest, project_dir, source_identity,
                                         session.opencode_session_id)) {
            continue;
        }
        sessions.push_back(std::move(session));
    }
    return sessions;
}

json import_metadata(const OpencodeSourceSession& source,
                     const std::string& message_id = {},
                     const std::string& part_id = {}) {
    json meta = {
        {"imported_from_opencode", {
            {"source_database", source.source_database},
            {"source_identity", source.source_identity},
            {"session_id", source.opencode_session_id},
        }}
    };
    if (!message_id.empty()) meta["imported_from_opencode"]["message_id"] = message_id;
    if (!part_id.empty()) meta["imported_from_opencode"]["part_id"] = part_id;
    return meta;
}

std::string join_with_blank_line(const std::vector<std::string>& chunks) {
    std::string out;
    for (const auto& chunk : chunks) {
        if (chunk.empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += chunk;
    }
    return out;
}

std::string tool_arguments_from_input(const json& state) {
    auto it = state.find("input");
    if (it == state.end() || it->is_null()) return "{}";
    if (it->is_string()) return it->get<std::string>();
    return it->dump(-1);
}

std::string tool_result_content(const json& state) {
    const std::string status = state.value("status", std::string{});
    if (status == "completed") {
        if (auto it = state.find("output"); it != state.end()) {
            if (it->is_string()) return it->get<std::string>();
            return it->dump(-1);
        }
    }
    if (auto it = state.find("error"); it != state.end()) {
        if (it->is_string()) return it->get<std::string>();
        return it->dump(-1);
    }
    return state.dump(-1);
}

struct OpencodePartRow {
    std::string id;
    std::string type;
    std::int64_t time_created_ms = 0;
    json data;
};

std::vector<OpencodePartRow> parts_for_message(sqlite3* db,
                                               const std::string& session_id,
                                               const std::string& message_id) {
    Statement stmt(db,
        "SELECT id, time_created, data "
        "FROM part "
        "WHERE session_id = ? AND message_id = ? "
        "ORDER BY time_created ASC, id ASC;");
    stmt.bind_text(1, session_id);
    stmt.bind_text(2, message_id);

    std::vector<OpencodePartRow> parts;
    while (stmt.step()) {
        OpencodePartRow row;
        row.id = stmt.text(0);
        row.time_created_ms = stmt.i64(1);
        row.data = parse_json_or_null(stmt.text(2));
        if (row.data.is_object()) row.type = row.data.value("type", std::string{});
        parts.push_back(std::move(row));
    }
    return parts;
}

bool is_ignored_structural_part(const std::string& type) {
    return type == "step-start" ||
           type == "step-finish" ||
           type == "snapshot" ||
           type == "patch";
}

std::string fallback_part_text(const OpencodePartRow& part) {
    std::string text = "[Imported opencode part";
    if (!part.type.empty()) text += ": " + part.type;
    text += "]";
    if (!part.data.is_null()) text += "\n" + part.data.dump(-1);
    return text;
}

std::vector<ChatMessage> convert_messages(sqlite3* db, const OpencodeSourceSession& source) {
    validate_opencode_schema(db);
    Statement stmt(db,
        "SELECT id, time_created, data "
        "FROM message "
        "WHERE session_id = ? "
        "ORDER BY time_created ASC, id ASC;");
    stmt.bind_text(1, source.opencode_session_id);

    std::vector<ChatMessage> converted;
    while (stmt.step()) {
        const std::string message_id = stmt.text(0);
        const std::int64_t message_time_ms = stmt.i64(1);
        const json message_data = parse_json_or_null(stmt.text(2));
        std::string role = message_data.is_object()
            ? message_data.value("role", std::string{})
            : std::string{};
        if (role != "user" && role != "assistant") role = "assistant";
        const std::string timestamp = iso8601_from_unix_ms(message_time_ms);

        std::vector<std::string> text_chunks;
        std::vector<std::string> reasoning_chunks;
        std::vector<json> tool_calls;
        struct ToolResult {
            std::string call_id;
            std::string content;
            std::string part_id;
            std::string status;
            std::string name;
        };
        std::vector<ToolResult> tool_results;
        std::vector<OpencodePartRow> unsupported_parts;

        for (const auto& part : parts_for_message(db, source.opencode_session_id, message_id)) {
            if (!part.data.is_object()) {
                unsupported_parts.push_back(part);
                continue;
            }
            if (part.type == "text") {
                const std::string text = part.data.value("text", std::string{});
                if (!text.empty()) text_chunks.push_back(text);
                continue;
            }
            if (part.type == "reasoning") {
                const std::string text = part.data.value("text", std::string{});
                if (!text.empty()) reasoning_chunks.push_back(text);
                continue;
            }
            if (part.type == "tool") {
                const json state = part.data.value("state", json::object());
                const std::string status = state.is_object()
                    ? state.value("status", std::string{})
                    : std::string{};
                if (status == "completed" || status == "error") {
                    const std::string call_id =
                        part.data.value("callID", part.id.empty() ? std::string{"opencode_tool"} : part.id);
                    const std::string name = part.data.value("tool", std::string{"opencode_tool"});
                    tool_calls.push_back(json{
                        {"id", call_id},
                        {"type", "function"},
                        {"function", {
                            {"name", name},
                            {"arguments", tool_arguments_from_input(state)},
                        }},
                    });
                    tool_results.push_back(ToolResult{
                        call_id,
                        tool_result_content(state),
                        part.id,
                        status,
                        name,
                    });
                } else {
                    unsupported_parts.push_back(part);
                }
                continue;
            }
            if (!is_ignored_structural_part(part.type)) {
                unsupported_parts.push_back(part);
            }
        }

        const std::string content = join_with_blank_line(text_chunks);
        const std::string reasoning = join_with_blank_line(reasoning_chunks);
        if (!content.empty() || !reasoning.empty() || !tool_calls.empty()) {
            ChatMessage msg;
            msg.role = role;
            msg.content = content;
            if (role == "assistant") {
                msg.reasoning_content = reasoning;
                if (!tool_calls.empty()) msg.tool_calls = tool_calls;
            }
            msg.uuid = message_id;
            msg.timestamp = timestamp;
            msg.metadata = import_metadata(source, message_id);
            converted.push_back(std::move(msg));
        }

        if (role == "assistant") {
            for (const auto& result : tool_results) {
                ChatMessage tool_msg;
                tool_msg.role = "tool";
                tool_msg.tool_call_id = result.call_id;
                tool_msg.content = result.content;
                tool_msg.timestamp = timestamp;
                tool_msg.metadata = import_metadata(source, message_id, result.part_id);
                tool_msg.metadata["imported_from_opencode"]["tool"] = result.name;
                tool_msg.metadata["imported_from_opencode"]["status"] = result.status;
                converted.push_back(std::move(tool_msg));
            }
        }

        for (const auto& part : unsupported_parts) {
            ChatMessage fallback;
            fallback.role = role;
            fallback.content = fallback_part_text(part);
            fallback.uuid = part.id;
            fallback.timestamp = iso8601_from_unix_ms(
                part.time_created_ms > 0 ? part.time_created_ms : message_time_ms);
            fallback.metadata = import_metadata(source, message_id, part.id);
            fallback.metadata["transcript_only"] = true;
            converted.push_back(std::move(fallback));
        }
    }
    return converted;
}

std::string first_user_summary(const std::vector<ChatMessage>& messages) {
    for (const auto& message : messages) {
        if (message.role != "user") continue;
        if (message.metadata.is_object() && message.metadata.value("transcript_only", false)) continue;
        if (!message.content.empty()) return truncate_utf8_prefix(message.content, 80);
    }
    return {};
}

std::string unique_acecode_session_id(const std::string& project_dir) {
    for (int i = 0; i < 100; ++i) {
        const std::string id = SessionStorage::generate_session_id();
        std::error_code ec;
        if (!fs::exists(path_from_utf8(SessionStorage::session_path(project_dir, id)), ec) &&
            !fs::exists(path_from_utf8(SessionStorage::meta_path(project_dir, id)), ec)) {
            return id;
        }
    }
    throw std::runtime_error("unable to allocate imported session id");
}

OpencodeImportSessionResult import_one_session(const OpencodeImportOptions& options,
                                               const OpencodeSourceSession& source) {
    OpencodeImportSessionResult result;
    result.opencode_session_id = source.opencode_session_id;
    result.title = source.title;
    try {
        SqliteDb db(source.source_database);
        auto messages = convert_messages(db.get(), source);
        if (messages.empty()) {
            result.skipped = true;
            return result;
        }

        const std::string id = unique_acecode_session_id(options.project_dir);
        SessionMeta meta;
        meta.id = id;
        meta.cwd = options.cwd;
        meta.created_at = iso8601_from_unix_ms(source.time_created_ms);
        meta.updated_at = iso8601_from_unix_ms(source.time_updated_ms);
        meta.message_count = static_cast<int>(messages.size());
        meta.summary = first_user_summary(messages);
        meta.provider = source.provider;
        meta.model = source.model;
        meta.title = source.title.empty()
            ? (meta.summary.empty() ? source.opencode_session_id : meta.summary)
            : source.title;
        meta.title_source = "imported";
        meta.turn_count = static_cast<int>(std::count_if(messages.begin(), messages.end(), [](const ChatMessage& message) {
            return message.role == "user" &&
                   !(message.metadata.is_object() && message.metadata.value("transcript_only", false));
        }));
        meta.no_workspace = false;

        SessionStorage::write_messages(SessionStorage::session_path(options.project_dir, id), messages);
        SessionStorage::write_meta(SessionStorage::meta_path(options.project_dir, id), meta);
        append_manifest_entry(options.project_dir, source, id);
        result.imported = true;
        result.acecode_session_id = id;
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

OpencodeImportPreview preview_from_candidates(const std::vector<std::string>& candidates,
                                              const std::string& cwd,
                                              const std::string& project_dir) {
    OpencodeImportPreview result;
    std::string first_error;
    for (const auto& candidate : candidates) {
        OpencodeImportPreview one = preview_opencode_import_from_database(candidate, cwd, project_dir);
        if (!one.error.empty() && first_error.empty()) first_error = one.error;
        if (result.source_database.empty() && !one.source_database.empty()) {
            result.source_database = one.source_database;
        }
        for (auto& session : one.sessions) {
            result.sessions.push_back(std::move(session));
        }
    }
    result.count = static_cast<int>(result.sessions.size());
    result.available = result.count > 0;
    if (!result.available) result.error = first_error;
    return result;
}

} // namespace

std::vector<std::string> discover_opencode_database_candidates() {
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seen;

    const std::string override_db = getenv_or_empty("OPENCODE_DB");
    if (!override_db.empty()) {
        if (override_db == ":memory:") {
            candidates.push_back(override_db);
            return candidates;
        }
        fs::path override_path = path_from_utf8(override_db);
        if (override_path.is_absolute()) {
            add_unique_path(candidates, seen, override_path, false);
            return candidates;
        }
        for (const auto& dir : opencode_data_dir_candidates()) {
            add_unique_path(candidates, seen, dir / override_path, false);
        }
        return candidates;
    }

    for (const auto& dir : opencode_data_dir_candidates()) {
        add_unique_path(candidates, seen, dir / "opencode.db", true);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = path_to_utf8(entry.path().filename());
            if (name.size() >= 12 &&
                name.rfind("opencode-", 0) == 0 &&
                name.size() > 3 &&
                name.substr(name.size() - 3) == ".db") {
                add_unique_path(candidates, seen, entry.path(), true);
            }
        }
    }

    return candidates;
}

OpencodeImportPreview preview_opencode_import(const std::string& cwd,
                                              const std::string& project_dir) {
    return preview_from_candidates(discover_opencode_database_candidates(), cwd, project_dir);
}

OpencodeImportPreview preview_opencode_import_from_database(
    const std::string& database_path,
    const std::string& cwd,
    const std::string& project_dir) {
    OpencodeImportPreview result;
    result.source_database = database_path;
    try {
        SqliteDb db(database_path);
        result.sessions = query_source_sessions(db.get(), database_path, cwd, project_dir);
        result.count = static_cast<int>(result.sessions.size());
        result.available = result.count > 0;
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

OpencodeImportJobStatus import_opencode_sessions(
    const OpencodeImportOptions& options,
    const OpencodeImportProgress& progress) {
    if (!options.source_database_override.empty()) {
        return import_opencode_sessions_from_database(options.source_database_override, options, progress);
    }

    OpencodeImportJobStatus status;
    status.workspace_hash = options.workspace_hash;
    status.state = "running";

    const auto preview = preview_opencode_import(options.cwd, options.project_dir);
    status.total = preview.count;
    if (status.total <= 0) {
        if (!preview.error.empty()) {
            status.state = "failed";
            status.error = preview.error;
        } else {
            status.state = "complete";
        }
        if (progress) progress(status);
        return status;
    }

    if (progress) progress(status);
    for (const auto& source : preview.sessions) {
        status.current_title = source.title.empty() ? source.opencode_session_id : source.title;
        if (progress) progress(status);
        auto result = import_one_session(options, source);
        if (result.imported) {
            status.imported += 1;
            status.session_ids.push_back(result.acecode_session_id);
        } else if (result.skipped) {
            status.skipped += 1;
        } else {
            status.failed += 1;
            if (!result.error.empty()) status.error = result.error;
        }
        if (progress) progress(status);
    }
    status.current_title.clear();
    status.state = status.failed > 0 && status.imported == 0 ? "failed" : "complete";
    if (progress) progress(status);
    return status;
}

OpencodeImportJobStatus import_opencode_sessions_from_database(
    const std::string& database_path,
    const OpencodeImportOptions& options,
    const OpencodeImportProgress& progress) {
    OpencodeImportJobStatus status;
    status.workspace_hash = options.workspace_hash;
    status.state = "running";

    const auto preview = preview_opencode_import_from_database(
        database_path, options.cwd, options.project_dir);
    status.total = preview.count;
    if (status.total <= 0) {
        if (!preview.error.empty()) {
            status.state = "failed";
            status.error = preview.error;
        } else {
            status.state = "complete";
        }
        if (progress) progress(status);
        return status;
    }

    if (progress) progress(status);
    for (const auto& source : preview.sessions) {
        status.current_title = source.title.empty() ? source.opencode_session_id : source.title;
        if (progress) progress(status);
        auto result = import_one_session(options, source);
        if (result.imported) {
            status.imported += 1;
            status.session_ids.push_back(result.acecode_session_id);
        } else if (result.skipped) {
            status.skipped += 1;
        } else {
            status.failed += 1;
            if (!result.error.empty()) status.error = result.error;
        }
        if (progress) progress(status);
    }
    status.current_title.clear();
    status.state = status.failed > 0 && status.imported == 0 ? "failed" : "complete";
    if (progress) progress(status);
    return status;
}

std::vector<ChatMessage> convert_opencode_session_messages_for_test(
    const std::string& database_path,
    const OpencodeSourceSession& source) {
    SqliteDb db(database_path);
    return convert_messages(db.get(), source);
}

} // namespace acecode
