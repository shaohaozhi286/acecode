#include "session_user_message_search.hpp"

#include "session_serializer.hpp"
#include "session_storage.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {
namespace {

constexpr int kDefaultSearchLimit = 50;
constexpr int kMaxSearchLimit = 100;
constexpr std::size_t kSnippetMaxBytes = 180;
constexpr std::size_t kSnippetContextBytes = 70;

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::string sqlite_error(sqlite3* db, const std::string& prefix) {
    std::ostringstream oss;
    oss << prefix;
    if (db) oss << ": " << sqlite3_errmsg(db);
    return oss.str();
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string trim_copy(const std::string& input) {
    std::size_t first = 0;
    while (first < input.size() &&
           std::isspace(static_cast<unsigned char>(input[first])) != 0) {
        ++first;
    }
    std::size_t last = input.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(input[last - 1])) != 0) {
        --last;
    }
    return input.substr(first, last - first);
}

std::string collapse_ws(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool in_ws = false;
    for (unsigned char c : input) {
        if (std::isspace(c) != 0) {
            if (!in_ws && !out.empty()) out.push_back(' ');
            in_ws = true;
            continue;
        }
        out.push_back(static_cast<char>(c));
        in_ws = false;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::size_t utf8_safe_prefix_length(const std::string& text, std::size_t max_bytes) {
    const std::size_t limit = (std::min)(max_bytes, text.size());
    std::size_t i = 0;
    std::size_t last_valid = 0;

    while (i < limit) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t seq_len = 0;
        if ((c & 0x80u) == 0) {
            seq_len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            seq_len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            seq_len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            seq_len = 4;
        } else {
            break;
        }
        if (i + seq_len > limit || i + seq_len > text.size()) break;
        bool valid = true;
        for (std::size_t j = 1; j < seq_len; ++j) {
            const unsigned char continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        i += seq_len;
        last_valid = i;
    }
    return last_valid;
}

std::string utf8_prefix(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    return text.substr(0, utf8_safe_prefix_length(text, max_bytes));
}

std::string make_snippet(const std::string& source, const std::string& query_norm) {
    const std::string collapsed = collapse_ws(source);
    if (collapsed.empty()) return {};
    const std::string norm = ascii_lower(collapsed);
    const std::size_t hit = query_norm.empty() ? std::string::npos : norm.find(query_norm);
    if (hit == std::string::npos || collapsed.size() <= kSnippetMaxBytes) {
        std::string snippet = utf8_prefix(collapsed, kSnippetMaxBytes);
        if (snippet.size() < collapsed.size()) snippet += "...";
        return snippet;
    }

    std::size_t start = hit > kSnippetContextBytes ? hit - kSnippetContextBytes : 0;
    start = utf8_safe_prefix_length(collapsed, start);
    std::size_t end = (std::min)(collapsed.size(), hit + query_norm.size() + kSnippetContextBytes);
    end = utf8_safe_prefix_length(collapsed, end);
    if (end < start) end = start;
    std::string snippet = collapsed.substr(start, end - start);
    if (start > 0) snippet = "..." + snippet;
    if (end < collapsed.size()) snippet += "...";
    return snippet;
}

std::string attachment_text_from_names(const std::vector<std::string>& names) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << names[i];
    }
    return oss.str();
}

nlohmann::json attachment_names_to_json(const std::vector<std::string>& names) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& name : names) arr.push_back(name);
    return arr;
}

std::vector<std::string> attachment_names_from_json(const std::string& raw) {
    std::vector<std::string> out;
    if (raw.empty()) return out;
    try {
        auto parsed = nlohmann::json::parse(raw);
        if (!parsed.is_array()) return out;
        for (const auto& item : parsed) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    } catch (...) {
    }
    return out;
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

bool bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

int match_score(const std::string& user_text,
                const std::string& attachment_text,
                const std::string& query_norm,
                int message_ordinal) {
    const bool user_hit = ascii_lower(user_text).find(query_norm) != std::string::npos;
    const bool attachment_hit = ascii_lower(attachment_text).find(query_norm) != std::string::npos;
    int score = 300;
    if (attachment_hit) score = 700;
    if (user_hit) score = 900;
    return score + (std::min)(message_ordinal, 99);
}

} // namespace

bool is_searchable_visible_user_message(const ChatMessage& msg) {
    return msg.role == "user" &&
           !msg.is_meta &&
           !msg.is_compact_summary &&
           !(msg.metadata.is_object() && msg.metadata.value("hidden_goal_context", false));
}

std::string searchable_user_message_text(const ChatMessage& msg) {
    if (!is_searchable_visible_user_message(msg)) return {};
    if (msg.metadata.is_object()) {
        auto it = msg.metadata.find("display_text");
        if (it != msg.metadata.end() && it->is_string()) {
            std::string display = trim_copy(it->get<std::string>());
            if (!display.empty()) return display;
        }
    }
    return msg.content;
}

std::vector<std::string> searchable_user_message_attachment_names(const ChatMessage& msg) {
    std::vector<std::string> out;
    if (!is_searchable_visible_user_message(msg)) return out;
    if (!msg.content_parts.is_array()) return out;
    std::unordered_set<std::string> seen;
    for (const auto& part : msg.content_parts) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", std::string{});
        if (type != "file" && type != "image") continue;
        auto it = part.find("attachment");
        if (it == part.end() || !it->is_object()) continue;
        auto name_it = it->find("name");
        if (name_it == it->end() || !name_it->is_string()) continue;
        std::string name = trim_copy(name_it->get<std::string>());
        if (name.empty()) continue;
        if (seen.insert(name).second) out.push_back(std::move(name));
    }
    return out;
}

std::optional<SearchableUserMessage> build_searchable_user_message(
    const std::string& session_id,
    int message_ordinal,
    const ChatMessage& msg) {
    if (!is_searchable_visible_user_message(msg)) return std::nullopt;
    SearchableUserMessage out;
    out.session_id = session_id;
    out.message_ordinal = message_ordinal;
    out.message_uuid = msg.uuid;
    out.user_text = searchable_user_message_text(msg);
    out.attachment_names = searchable_user_message_attachment_names(msg);
    out.attachment_text = attachment_text_from_names(out.attachment_names);
    out.search_text = out.user_text;
    if (!out.attachment_text.empty()) {
        if (!out.search_text.empty()) out.search_text += "\n";
        out.search_text += out.attachment_text;
    }
    out.search_text_norm = ascii_lower(out.search_text);
    out.snippet_text = out.user_text.empty() ? out.attachment_text : out.user_text;
    if (trim_copy(out.search_text).empty()) return std::nullopt;
    return out;
}

SessionUserMessageFileSignature session_user_message_file_signature(
    const std::string& jsonl_path) {
    SessionUserMessageFileSignature out;
    if (jsonl_path.empty()) return out;
    std::error_code ec;
    const fs::path path = path_from_utf8(jsonl_path);
    if (!fs::exists(path, ec) || ec) return out;
    out.exists = true;
    out.size = static_cast<std::int64_t>(fs::file_size(path, ec));
    if (ec) {
        out.size = 0;
        ec.clear();
    }
    out.mtime = fs::last_write_time(path, ec).time_since_epoch().count();
    if (ec) out.mtime = 0;
    return out;
}

SessionUserMessageIndex::SessionUserMessageIndex(std::string project_dir)
    : project_dir_(std::move(project_dir)) {}

SessionUserMessageIndex::~SessionUserMessageIndex() {
    if (db_) sqlite3_close(db_);
}

bool SessionUserMessageIndex::open(std::string* error) {
    if (db_) return true;
    if (project_dir_.empty()) {
        set_error(error, "project directory required");
        return false;
    }
    std::error_code ec;
    fs::create_directories(path_from_utf8(project_dir_), ec);
    if (ec) {
        set_error(error, "failed to create project directory: " + ec.message());
        return false;
    }
    // 独立于 goal 的 state.sqlite3:SessionManager::existing_goal_store() 以
    // 「state.sqlite3 文件存在」作为 goal store 的惰性打开条件。如果索引也
    // 写进 state.sqlite3,每个普通会话的第一条消息就会把文件造出来,goal
    // store 随之在每个 turn 打开常驻连接(Windows 下还会锁住项目目录)。
    // 索引连接是栈对象短开短关,单独一个 db 文件不改变任何既有约定。
    const std::string db_path =
        path_to_utf8(path_from_utf8(project_dir_) / "user_message_search.sqlite3");
    if (sqlite3_open_v2(db_path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        nullptr) != SQLITE_OK) {
        set_error(error, sqlite_error(db_, "failed to open session search database"));
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    return true;
}

bool SessionUserMessageIndex::exec(const char* sql, std::string* error) {
    char* raw_error = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &raw_error);
    if (rc != SQLITE_OK) {
        std::string msg = raw_error ? raw_error : sqlite3_errmsg(db_);
        sqlite3_free(raw_error);
        set_error(error, msg);
        return false;
    }
    return true;
}

bool SessionUserMessageIndex::initialize(std::string* error) {
    if (!open(error)) return false;
    constexpr const char* schema_sql =
        "CREATE TABLE IF NOT EXISTS session_user_message_index ("
        "session_id TEXT NOT NULL,"
        "message_ordinal INTEGER NOT NULL,"
        "message_uuid TEXT NOT NULL DEFAULT '',"
        "user_text TEXT NOT NULL DEFAULT '',"
        "attachment_text TEXT NOT NULL DEFAULT '',"
        "attachment_names_json TEXT NOT NULL DEFAULT '[]',"
        "search_text TEXT NOT NULL DEFAULT '',"
        "search_text_norm TEXT NOT NULL DEFAULT '',"
        "snippet_text TEXT NOT NULL DEFAULT '',"
        "PRIMARY KEY(session_id, message_ordinal)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_session_user_message_session "
        "ON session_user_message_index(session_id, message_ordinal DESC);"
        "CREATE TABLE IF NOT EXISTS session_user_message_sources ("
        "session_id TEXT PRIMARY KEY,"
        "jsonl_path TEXT NOT NULL,"
        "jsonl_mtime INTEGER NOT NULL,"
        "jsonl_size INTEGER NOT NULL,"
        "indexed_at_ms INTEGER NOT NULL"
        ");";
    if (!exec(schema_sql, error)) return false;
    return true;
}

bool SessionUserMessageIndex::source_matches_signature(
    const std::string& session_id,
    const SessionUserMessageFileSignature& signature,
    std::string* error) {
    if (!initialize(error)) return false;
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "SELECT jsonl_mtime, jsonl_size FROM session_user_message_sources "
        "WHERE session_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite_error(db_, "prepare source read failed"));
        return false;
    }
    bind_text(stmt, 1, session_id);
    const int rc = sqlite3_step(stmt);
    bool matches = false;
    if (rc == SQLITE_ROW) {
        matches = signature.exists &&
                  sqlite3_column_int64(stmt, 0) == signature.mtime &&
                  sqlite3_column_int64(stmt, 1) == signature.size;
    } else if (rc != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "source read failed"));
    }
    sqlite3_finalize(stmt);
    return matches;
}

bool SessionUserMessageIndex::update_source(
    const std::string& session_id,
    const std::string& jsonl_path,
    const SessionUserMessageFileSignature& signature,
    std::string* error) {
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "INSERT INTO session_user_message_sources("
        "session_id, jsonl_path, jsonl_mtime, jsonl_size, indexed_at_ms"
        ") VALUES(?, ?, ?, ?, ?) "
        "ON CONFLICT(session_id) DO UPDATE SET "
        "jsonl_path = excluded.jsonl_path,"
        "jsonl_mtime = excluded.jsonl_mtime,"
        "jsonl_size = excluded.jsonl_size,"
        "indexed_at_ms = excluded.indexed_at_ms;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite_error(db_, "prepare source update failed"));
        return false;
    }
    bool ok = bind_text(stmt, 1, session_id) &&
              bind_text(stmt, 2, jsonl_path) &&
              sqlite3_bind_int64(stmt, 3, signature.mtime) == SQLITE_OK &&
              sqlite3_bind_int64(stmt, 4, signature.size) == SQLITE_OK &&
              sqlite3_bind_int64(stmt, 5, now_ms()) == SQLITE_OK;
    if (ok && sqlite3_step(stmt) != SQLITE_DONE) {
        ok = false;
        set_error(error, sqlite_error(db_, "source update failed"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool SessionUserMessageIndex::upsert_searchable_message(
    const SearchableUserMessage& message,
    std::string* error) {
    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "INSERT INTO session_user_message_index("
        "session_id, message_ordinal, message_uuid, user_text, attachment_text, "
        "attachment_names_json, search_text, search_text_norm, snippet_text"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(session_id, message_ordinal) DO UPDATE SET "
        "message_uuid = excluded.message_uuid,"
        "user_text = excluded.user_text,"
        "attachment_text = excluded.attachment_text,"
        "attachment_names_json = excluded.attachment_names_json,"
        "search_text = excluded.search_text,"
        "search_text_norm = excluded.search_text_norm,"
        "snippet_text = excluded.snippet_text;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite_error(db_, "prepare message upsert failed"));
        return false;
    }
    const std::string names_json = attachment_names_to_json(message.attachment_names).dump();
    bool ok = bind_text(stmt, 1, message.session_id) &&
              sqlite3_bind_int(stmt, 2, message.message_ordinal) == SQLITE_OK &&
              bind_text(stmt, 3, message.message_uuid) &&
              bind_text(stmt, 4, message.user_text) &&
              bind_text(stmt, 5, message.attachment_text) &&
              bind_text(stmt, 6, names_json) &&
              bind_text(stmt, 7, message.search_text) &&
              bind_text(stmt, 8, message.search_text_norm) &&
              bind_text(stmt, 9, message.snippet_text);
    if (ok && sqlite3_step(stmt) != SQLITE_DONE) {
        ok = false;
        set_error(error, sqlite_error(db_, "message upsert failed"));
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool SessionUserMessageIndex::index_appended_message(
    const std::string& session_id,
    int message_ordinal,
    const ChatMessage& msg,
    const std::string& jsonl_path,
    const SessionUserMessageFileSignature& before_append,
    std::string* error) {
    if (session_id.empty() || jsonl_path.empty()) return true;
    if (!initialize(error)) return false;
    const auto after = session_user_message_file_signature(jsonl_path);
    const bool source_was_fresh = source_matches_signature(session_id, before_append, error);
    if (!source_was_fresh) {
        return rebuild_session(session_id, jsonl_path, error);
    }

    if (auto searchable = build_searchable_user_message(session_id, message_ordinal, msg)) {
        if (!upsert_searchable_message(*searchable, error)) return false;
    }
    return update_source(session_id, jsonl_path, after, error);
}

bool SessionUserMessageIndex::rebuild_session(
    const std::string& session_id,
    const std::string& jsonl_path,
    const std::vector<ChatMessage>& messages,
    std::string* error) {
    if (session_id.empty() || jsonl_path.empty()) return true;
    if (!initialize(error)) return false;
    const auto signature = session_user_message_file_signature(jsonl_path);
    if (!exec("BEGIN IMMEDIATE TRANSACTION;", error)) return false;

    bool ok = true;
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "DELETE FROM session_user_message_index WHERE session_id = ?;",
                           -1, &del, nullptr) != SQLITE_OK) {
        set_error(error, sqlite_error(db_, "prepare message delete failed"));
        ok = false;
    }
    if (ok) {
        bind_text(del, 1, session_id);
        if (sqlite3_step(del) != SQLITE_DONE) {
            set_error(error, sqlite_error(db_, "message delete failed"));
            ok = false;
        }
    }
    if (del) sqlite3_finalize(del);

    if (ok) {
        for (std::size_t i = 0; i < messages.size(); ++i) {
            auto searchable = build_searchable_user_message(
                session_id, static_cast<int>(i), messages[i]);
            if (!searchable.has_value()) continue;
            if (!upsert_searchable_message(*searchable, error)) {
                ok = false;
                break;
            }
        }
    }
    if (ok) ok = update_source(session_id, jsonl_path, signature, error);

    if (ok) {
        if (!exec("COMMIT;", error)) ok = false;
    } else {
        std::string ignored;
        exec("ROLLBACK;", &ignored);
    }
    return ok;
}

bool SessionUserMessageIndex::rebuild_session(
    const std::string& session_id,
    const std::string& jsonl_path,
    std::string* error) {
    return rebuild_session(session_id, jsonl_path,
                           SessionStorage::load_messages(jsonl_path),
                           error);
}

bool SessionUserMessageIndex::ensure_session_indexed(
    const std::string& session_id,
    const std::string& jsonl_path,
    std::string* error) {
    if (session_id.empty() || jsonl_path.empty()) return true;
    if (!initialize(error)) return false;
    const auto signature = session_user_message_file_signature(jsonl_path);
    if (source_matches_signature(session_id, signature, error)) return true;
    return rebuild_session(session_id, jsonl_path, error);
}

bool SessionUserMessageIndex::remove_session(const std::string& session_id,
                                             std::string* error) {
    if (session_id.empty()) return true;
    if (!initialize(error)) return false;
    constexpr const char* sqls[] = {
        "DELETE FROM session_user_message_index WHERE session_id = ?;",
        "DELETE FROM session_user_message_sources WHERE session_id = ?;",
    };
    for (const char* sql : sqls) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            set_error(error, sqlite_error(db_, "prepare session index removal failed"));
            return false;
        }
        bool ok = bind_text(stmt, 1, session_id) &&
                  sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok) set_error(error, sqlite_error(db_, "session index removal failed"));
        sqlite3_finalize(stmt);
        if (!ok) return false;
    }
    return true;
}

void SessionUserMessageIndex::prune_removed_sessions() {
    // 孤儿回收:sources 里记录的 JSONL 已从磁盘消失(purge / 手工删除),
    // 索引里的消息全文必须跟着删,否则已删除会话的用户输入会残留在
    // state.sqlite3 且永远不会被重建逻辑清理。
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT session_id, jsonl_path FROM session_user_message_sources;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    std::vector<std::string> removed;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string session_id = column_text(stmt, 0);
        const std::string jsonl_path = column_text(stmt, 1);
        if (!session_user_message_file_signature(jsonl_path).exists) {
            removed.push_back(session_id);
        }
    }
    sqlite3_finalize(stmt);
    for (const auto& session_id : removed) {
        std::string ignored;
        remove_session(session_id, &ignored);
    }
}

bool SessionUserMessageIndex::ensure_project_indexed(std::string* error) {
    if (!initialize(error)) return false;
    for (const auto& meta : SessionStorage::list_sessions(project_dir_)) {
        auto candidates = SessionStorage::find_session_files(project_dir_, meta.id);
        if (candidates.empty()) continue;
        std::string session_error;
        if (!ensure_session_indexed(meta.id, candidates.front().jsonl_path,
                                    &session_error)) {
            // 单个 session 的失败(文件被锁/损坏)不挡其它 session 的搜索。
            LOG_WARN("[session-search] index refresh skipped session " + meta.id +
                     ": " + session_error);
        }
    }
    prune_removed_sessions();
    return true;
}

std::vector<SessionUserMessageSearchResult> SessionUserMessageIndex::search(
    const std::string& query,
    int limit,
    std::string* error) {
    std::vector<SessionUserMessageSearchResult> out;
    const std::string query_norm = ascii_lower(trim_copy(query));
    if (query_norm.empty()) return out;
    if (limit <= 0) limit = kDefaultSearchLimit;
    limit = (std::min)(limit, kMaxSearchLimit);
    if (!initialize(error)) return out;

    sqlite3_stmt* stmt = nullptr;
    constexpr const char* sql =
        "SELECT i.session_id, i.message_ordinal, i.user_text, i.attachment_text, "
        "i.attachment_names_json, i.snippet_text "
        "FROM session_user_message_index i "
        "JOIN ("
        "SELECT session_id, MAX(message_ordinal) AS message_ordinal "
        "FROM session_user_message_index "
        "WHERE instr(search_text_norm, ?) > 0 "
        "GROUP BY session_id"
        ") latest ON latest.session_id = i.session_id "
        "AND latest.message_ordinal = i.message_ordinal "
        "ORDER BY i.message_ordinal DESC "
        "LIMIT ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        set_error(error, sqlite_error(db_, "prepare user message search failed"));
        return out;
    }
    bind_text(stmt, 1, query_norm);
    sqlite3_bind_int(stmt, 2, limit * 8);

    int step_rc = SQLITE_OK;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const std::string session_id = column_text(stmt, 0);
        const int ordinal = sqlite3_column_int(stmt, 1);
        const std::string user_text = column_text(stmt, 2);
        const std::string attachment_text = column_text(stmt, 3);
        const auto names = attachment_names_from_json(column_text(stmt, 4));
        std::string snippet_source = column_text(stmt, 5);
        if (ascii_lower(snippet_source).find(query_norm) == std::string::npos &&
            ascii_lower(attachment_text).find(query_norm) != std::string::npos) {
            snippet_source = attachment_text;
        }

        SessionUserMessageSearchResult result;
        result.session_id = session_id;
        result.message_ordinal = ordinal;
        result.score = match_score(user_text, attachment_text, query_norm, ordinal);
        result.snippet = make_snippet(snippet_source, query_norm);
        for (const auto& name : names) {
            if (ascii_lower(name).find(query_norm) != std::string::npos) {
                result.matched_attachment_names.push_back(name);
            }
        }
        out.push_back(std::move(result));
    }
    const bool completed = step_rc == SQLITE_DONE || step_rc == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!completed) {
        set_error(error, sqlite_error(db_, "user message search failed"));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.message_ordinal > b.message_ordinal;
              });
    if (static_cast<int>(out.size()) > limit) {
        out.erase(out.begin() + limit, out.end());
    }
    return out;
}

} // namespace acecode
