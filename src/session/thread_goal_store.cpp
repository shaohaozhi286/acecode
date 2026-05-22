#include "thread_goal_store.hpp"

#include "session_storage.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace acecode {

namespace {

constexpr int kSchemaVersion = 2;
constexpr std::size_t kMaxObjectiveBytes = 4000;
constexpr const char* kThreadGoalsCreateSql =
    "CREATE TABLE IF NOT EXISTS thread_goals ("
    "thread_id TEXT PRIMARY KEY,"
    "goal_id TEXT NOT NULL,"
    "objective TEXT NOT NULL,"
    "status TEXT NOT NULL CHECK(status IN ("
        "'active','paused','blocked','usage_limited','budget_limited','complete'"
    ")),"
    "token_budget INTEGER NULL,"
    "tokens_used INTEGER NOT NULL DEFAULT 0,"
    "time_used_seconds INTEGER NOT NULL DEFAULT 0,"
    "created_at_ms INTEGER NOT NULL,"
    "updated_at_ms INTEGER NOT NULL"
    ");";

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::string sqlite_error(sqlite3* db, const std::string& prefix) {
    std::ostringstream oss;
    oss << prefix;
    if (db) oss << ": " << sqlite3_errmsg(db);
    return oss.str();
}

bool exec_sql(sqlite3* db, const char* sql, std::string* error) {
    char* raw_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw_error);
    if (rc == SQLITE_OK) return true;
    std::string msg = raw_error ? raw_error : sqlite3_errmsg(db);
    sqlite3_free(raw_error);
    set_error(error, msg);
    return false;
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql, std::string* error) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            set_error(error, sqlite_error(db_, "prepare failed"));
        }
    }

    ~Statement() {
        if (stmt_) sqlite3_finalize(stmt_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return stmt_; }
    explicit operator bool() const { return stmt_ != nullptr; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

bool bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

bool bind_i64(sqlite3_stmt* stmt, int index, std::int64_t value) {
    return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

bool bind_optional_i64(sqlite3_stmt* stmt, int index, std::optional<std::int64_t> value) {
    if (!value.has_value()) return sqlite3_bind_null(stmt, index) == SQLITE_OK;
    return bind_i64(stmt, index, *value);
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}

std::optional<std::int64_t> column_optional_i64(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) return std::nullopt;
    return static_cast<std::int64_t>(sqlite3_column_int64(stmt, index));
}

bool thread_goal_table_supports_stopped_statuses(sqlite3* db, std::string* error) {
    Statement stmt(db,
        "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = 'thread_goals';",
        error);
    if (!stmt) return false;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        const std::string sql = column_text(stmt.get(), 0);
        return sql.find("'blocked'") != std::string::npos &&
               sql.find("'usage_limited'") != std::string::npos;
    }
    if (rc != SQLITE_DONE) set_error(error, sqlite_error(db, "schema read failed"));
    return false;
}

bool migrate_thread_goal_statuses_v2(sqlite3* db, std::string* error) {
    return exec_sql(db,
        "DROP TABLE IF EXISTS thread_goals_new;"
        "CREATE TABLE thread_goals_new ("
        "thread_id TEXT PRIMARY KEY,"
        "goal_id TEXT NOT NULL,"
        "objective TEXT NOT NULL,"
        "status TEXT NOT NULL CHECK(status IN ("
            "'active','paused','blocked','usage_limited','budget_limited','complete'"
        ")),"
        "token_budget INTEGER NULL,"
        "tokens_used INTEGER NOT NULL DEFAULT 0,"
        "time_used_seconds INTEGER NOT NULL DEFAULT 0,"
        "created_at_ms INTEGER NOT NULL,"
        "updated_at_ms INTEGER NOT NULL"
        ");"
        "INSERT INTO thread_goals_new("
        "thread_id, goal_id, objective, status, token_budget, tokens_used, "
        "time_used_seconds, created_at_ms, updated_at_ms"
        ") SELECT "
        "thread_id, goal_id, objective, status, token_budget, tokens_used, "
        "time_used_seconds, created_at_ms, updated_at_ms "
        "FROM thread_goals;"
        "DROP TABLE thread_goals;"
        "ALTER TABLE thread_goals_new RENAME TO thread_goals;",
        error);
}

bool record_schema_version(sqlite3* db, int version, std::int64_t applied_at_ms, std::string* error) {
    Statement stmt(db,
        "INSERT OR IGNORE INTO schema_migrations(version, applied_at_ms) VALUES(?, ?);",
        error);
    return stmt &&
           bind_i64(stmt.get(), 1, version) &&
           bind_i64(stmt.get(), 2, applied_at_ms) &&
           sqlite3_step(stmt.get()) == SQLITE_DONE;
}

ThreadGoal row_to_goal(sqlite3_stmt* stmt) {
    ThreadGoal goal;
    goal.thread_id = column_text(stmt, 0);
    goal.goal_id = column_text(stmt, 1);
    goal.objective = column_text(stmt, 2);
    goal.status = parse_thread_goal_status(column_text(stmt, 3)).value_or(ThreadGoalStatus::Paused);
    goal.token_budget = column_optional_i64(stmt, 4);
    goal.tokens_used = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 5));
    goal.time_used_seconds = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 6));
    goal.created_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 7));
    goal.updated_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 8));
    return goal;
}

} // namespace

std::string to_string(ThreadGoalStatus status) {
    switch (status) {
        case ThreadGoalStatus::Active: return "active";
        case ThreadGoalStatus::Paused: return "paused";
        case ThreadGoalStatus::Blocked: return "blocked";
        case ThreadGoalStatus::UsageLimited: return "usage_limited";
        case ThreadGoalStatus::BudgetLimited: return "budget_limited";
        case ThreadGoalStatus::Complete: return "complete";
    }
    return "paused";
}

std::optional<ThreadGoalStatus> parse_thread_goal_status(const std::string& status) {
    if (status == "active") return ThreadGoalStatus::Active;
    if (status == "paused") return ThreadGoalStatus::Paused;
    if (status == "blocked") return ThreadGoalStatus::Blocked;
    if (status == "usage_limited") return ThreadGoalStatus::UsageLimited;
    if (status == "budget_limited") return ThreadGoalStatus::BudgetLimited;
    if (status == "complete") return ThreadGoalStatus::Complete;
    return std::nullopt;
}

bool is_thread_goal_active(ThreadGoalStatus status) {
    return status == ThreadGoalStatus::Active;
}

bool is_thread_goal_terminal(ThreadGoalStatus status) {
    return status == ThreadGoalStatus::BudgetLimited || status == ThreadGoalStatus::Complete;
}

std::string trim_goal_objective(const std::string& text) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t begin = 0;
    while (begin < text.size() && is_space(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

bool validate_goal_objective(const std::string& text, std::string* error) {
    const std::string trimmed = trim_goal_objective(text);
    if (trimmed.empty()) {
        set_error(error, "Goal objective cannot be empty.");
        return false;
    }
    if (trimmed.size() > kMaxObjectiveBytes) {
        set_error(error, "Goal objective is too long.");
        return false;
    }
    return true;
}

bool validate_goal_token_budget(std::optional<std::int64_t> budget, std::string* error) {
    if (budget.has_value() && *budget <= 0) {
        set_error(error, "Goal token budget must be a positive integer.");
        return false;
    }
    return true;
}

nlohmann::json thread_goal_to_json(const ThreadGoal& goal) {
    nlohmann::json out;
    out["thread_id"] = goal.thread_id;
    out["goal_id"] = goal.goal_id;
    out["objective"] = goal.objective;
    out["status"] = to_string(goal.status);
    if (goal.token_budget.has_value()) {
        out["token_budget"] = *goal.token_budget;
        out["remaining_tokens"] = std::max<std::int64_t>(0, *goal.token_budget - goal.tokens_used);
    } else {
        out["token_budget"] = nullptr;
        out["remaining_tokens"] = nullptr;
    }
    out["tokens_used"] = goal.tokens_used;
    out["time_used_seconds"] = goal.time_used_seconds;
    out["created_at_ms"] = goal.created_at_ms;
    out["updated_at_ms"] = goal.updated_at_ms;
    return out;
}

ThreadGoalStore::ThreadGoalStore(std::filesystem::path project_dir)
    : db_path_(database_path_for_project(project_dir)) {}

ThreadGoalStore::~ThreadGoalStore() {
    if (db_) sqlite3_close(db_);
}

std::filesystem::path ThreadGoalStore::database_path_for_project(
    const std::filesystem::path& project_dir) {
    return project_dir / "state.sqlite3";
}

bool ThreadGoalStore::initialize(std::string* error) {
    if (db_) return true;

    std::error_code ec;
    std::filesystem::create_directories(db_path_.parent_path(), ec);
    if (ec) {
        set_error(error, "failed to create project state directory: " + ec.message());
        return false;
    }

    const std::string path = db_path_.u8string();
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        set_error(error, sqlite_error(db_, "failed to open goal state database"));
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    if (!exec_sql(db_, "PRAGMA foreign_keys = ON;", error)) return false;
    if (!exec_sql(db_,
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "version INTEGER PRIMARY KEY,"
        "applied_at_ms INTEGER NOT NULL"
        ");",
        error)) return false;

    if (!exec_sql(db_, "BEGIN IMMEDIATE;", error)) return false;
    bool ok = exec_sql(db_, kThreadGoalsCreateSql, error);
    if (ok) {
        std::string schema_error;
        const bool supports_stopped =
            thread_goal_table_supports_stopped_statuses(db_, &schema_error);
        if (!schema_error.empty()) {
            set_error(error, schema_error);
            ok = false;
        } else if (!supports_stopped) {
            ok = migrate_thread_goal_statuses_v2(db_, error);
        }
    }
    if (ok) {
        const std::int64_t applied_at = now_ms();
        ok = record_schema_version(db_, 1, applied_at, error) &&
             record_schema_version(db_, kSchemaVersion, applied_at, error);
        if (!ok && error && error->empty()) {
            *error = sqlite_error(db_, "migration insert failed");
        }
    }
    if (ok) {
        ok = exec_sql(db_, "COMMIT;", error);
    } else {
        std::string rollback_error;
        exec_sql(db_, "ROLLBACK;", &rollback_error);
    }
    return ok;
}

bool ThreadGoalStore::ensure_initialized(std::string* error) const {
    if (db_) return true;
    set_error(error, "goal store is not initialized");
    return false;
}

std::optional<ThreadGoal> ThreadGoalStore::get_thread_goal(
    const std::string& thread_id,
    std::string* error) const {
    if (!ensure_initialized(error)) return std::nullopt;
    Statement stmt(db_,
        "SELECT thread_id, goal_id, objective, status, token_budget, tokens_used, "
        "time_used_seconds, created_at_ms, updated_at_ms "
        "FROM thread_goals WHERE thread_id = ?;",
        error);
    if (!stmt || !bind_text(stmt.get(), 1, thread_id)) {
        if (error && error->empty()) *error = sqlite_error(db_, "goal read failed");
        return std::nullopt;
    }
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) return row_to_goal(stmt.get());
    if (rc != SQLITE_DONE) set_error(error, sqlite_error(db_, "goal read failed"));
    return std::nullopt;
}

bool ThreadGoalStore::replace_thread_goal(const std::string& thread_id,
                                          const std::string& objective,
                                          std::optional<std::int64_t> token_budget,
                                          ThreadGoalStatus status,
                                          std::string* error) {
    if (!ensure_initialized(error)) return false;
    std::string validation_error;
    const std::string trimmed = trim_goal_objective(objective);
    if (!validate_goal_objective(trimmed, &validation_error) ||
        !validate_goal_token_budget(token_budget, &validation_error)) {
        set_error(error, validation_error);
        return false;
    }

    const std::int64_t now = now_ms();
    Statement stmt(db_,
        "INSERT INTO thread_goals("
        "thread_id, goal_id, objective, status, token_budget, tokens_used, "
        "time_used_seconds, created_at_ms, updated_at_ms"
        ") VALUES(?, ?, ?, ?, ?, 0, 0, ?, ?) "
        "ON CONFLICT(thread_id) DO UPDATE SET "
        "goal_id = excluded.goal_id,"
        "objective = excluded.objective,"
        "status = excluded.status,"
        "token_budget = excluded.token_budget,"
        "tokens_used = 0,"
        "time_used_seconds = 0,"
        "created_at_ms = excluded.created_at_ms,"
        "updated_at_ms = excluded.updated_at_ms;",
        error);
    if (!stmt) return false;

    const std::string goal_id = SessionStorage::generate_session_id();
    const bool bound =
        bind_text(stmt.get(), 1, thread_id) &&
        bind_text(stmt.get(), 2, goal_id) &&
        bind_text(stmt.get(), 3, trimmed) &&
        bind_text(stmt.get(), 4, to_string(status)) &&
        bind_optional_i64(stmt.get(), 5, token_budget) &&
        bind_i64(stmt.get(), 6, now) &&
        bind_i64(stmt.get(), 7, now);
    if (!bound) {
        set_error(error, sqlite_error(db_, "goal bind failed"));
        return false;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "goal replace failed"));
        return false;
    }
    return true;
}

bool ThreadGoalStore::update_thread_goal_status(const std::string& thread_id,
                                                const std::string& goal_id,
                                                ThreadGoalStatus status,
                                                std::string* error) {
    if (!ensure_initialized(error)) return false;
    Statement stmt(db_,
        "UPDATE thread_goals SET status = ?, updated_at_ms = ? "
        "WHERE thread_id = ? AND goal_id = ?;",
        error);
    if (!stmt) return false;
    const bool bound =
        bind_text(stmt.get(), 1, to_string(status)) &&
        bind_i64(stmt.get(), 2, now_ms()) &&
        bind_text(stmt.get(), 3, thread_id) &&
        bind_text(stmt.get(), 4, goal_id);
    if (!bound) {
        set_error(error, sqlite_error(db_, "goal status bind failed"));
        return false;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "goal status update failed"));
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool ThreadGoalStore::update_thread_goal_objective(
    const std::string& thread_id,
    const std::string& goal_id,
    const std::string& objective,
    std::optional<std::int64_t> token_budget,
    std::string* error) {
    if (!ensure_initialized(error)) return false;
    std::string validation_error;
    const std::string trimmed = trim_goal_objective(objective);
    if (!validate_goal_objective(trimmed, &validation_error) ||
        !validate_goal_token_budget(token_budget, &validation_error)) {
        set_error(error, validation_error);
        return false;
    }

    Statement stmt(db_,
        "UPDATE thread_goals SET objective = ?, token_budget = ?, updated_at_ms = ? "
        "WHERE thread_id = ? AND goal_id = ?;",
        error);
    if (!stmt) return false;
    const bool bound =
        bind_text(stmt.get(), 1, trimmed) &&
        bind_optional_i64(stmt.get(), 2, token_budget) &&
        bind_i64(stmt.get(), 3, now_ms()) &&
        bind_text(stmt.get(), 4, thread_id) &&
        bind_text(stmt.get(), 5, goal_id);
    if (!bound) {
        set_error(error, sqlite_error(db_, "goal objective bind failed"));
        return false;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "goal objective update failed"));
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool ThreadGoalStore::pause_active_thread_goal(const std::string& thread_id,
                                               std::string* error) {
    if (!ensure_initialized(error)) return false;
    Statement stmt(db_,
        "UPDATE thread_goals SET status = 'paused', updated_at_ms = ? "
        "WHERE thread_id = ? AND status = 'active';",
        error);
    if (!stmt) return false;
    if (!bind_i64(stmt.get(), 1, now_ms()) || !bind_text(stmt.get(), 2, thread_id)) {
        set_error(error, sqlite_error(db_, "goal pause bind failed"));
        return false;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "goal pause failed"));
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool ThreadGoalStore::delete_thread_goal(const std::string& thread_id,
                                         std::string* error) {
    if (!ensure_initialized(error)) return false;
    Statement stmt(db_, "DELETE FROM thread_goals WHERE thread_id = ?;", error);
    if (!stmt) return false;
    if (!bind_text(stmt.get(), 1, thread_id)) {
        set_error(error, sqlite_error(db_, "goal delete bind failed"));
        return false;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "goal delete failed"));
        return false;
    }
    return true;
}

ThreadGoalAccountingResult ThreadGoalStore::account_thread_goal_usage(
    const std::string& thread_id,
    const std::string& goal_id,
    std::int64_t token_delta,
    std::int64_t time_delta_seconds,
    bool allow_complete,
    std::string* error) {
    ThreadGoalAccountingResult result;
    if (!ensure_initialized(error)) return result;

    token_delta = std::max<std::int64_t>(0, token_delta);
    time_delta_seconds = std::max<std::int64_t>(0, time_delta_seconds);
    auto before = get_thread_goal(thread_id, error);
    if (!before.has_value() || before->goal_id != goal_id) return result;
    if (before->status != ThreadGoalStatus::Active &&
        !(allow_complete && before->status == ThreadGoalStatus::Complete)) {
        result.goal = before;
        return result;
    }
    if (token_delta == 0 && time_delta_seconds == 0) {
        result.goal = before;
        return result;
    }

    const std::int64_t next_tokens = before->tokens_used + token_delta;
    const std::int64_t next_time = before->time_used_seconds + time_delta_seconds;
    ThreadGoalStatus next_status = before->status;
    if (before->status == ThreadGoalStatus::Active &&
        before->token_budget.has_value() &&
        next_tokens >= *before->token_budget) {
        next_status = ThreadGoalStatus::BudgetLimited;
        result.became_budget_limited = true;
    }

    Statement stmt(db_,
        "UPDATE thread_goals SET tokens_used = ?, time_used_seconds = ?, "
        "status = ?, updated_at_ms = ? "
        "WHERE thread_id = ? AND goal_id = ?;",
        error);
    if (!stmt) return result;
    const bool bound =
        bind_i64(stmt.get(), 1, next_tokens) &&
        bind_i64(stmt.get(), 2, next_time) &&
        bind_text(stmt.get(), 3, to_string(next_status)) &&
        bind_i64(stmt.get(), 4, now_ms()) &&
        bind_text(stmt.get(), 5, thread_id) &&
        bind_text(stmt.get(), 6, goal_id);
    if (!bound) {
        set_error(error, sqlite_error(db_, "goal account bind failed"));
        return result;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, sqlite_error(db_, "goal account failed"));
        return result;
    }
    result.updated = sqlite3_changes(db_) > 0;
    result.goal = get_thread_goal(thread_id, error);
    return result;
}

bool ThreadGoalStore::copy_goal_reset_usage(const std::string& from_thread_id,
                                            const std::string& to_thread_id,
                                            std::string* error) {
    if (!ensure_initialized(error)) return false;
    auto existing = get_thread_goal(from_thread_id, error);
    if (!existing.has_value()) return true;
    return replace_thread_goal(to_thread_id,
                               existing->objective,
                               existing->token_budget,
                               existing->status,
                               error);
}

} // namespace acecode
