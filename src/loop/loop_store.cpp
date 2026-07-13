#include "loop_store.hpp"

#include "../config/config.hpp"
#include "../utils/utf8_path.hpp"
#include "../utils/uuid.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace acecode::loop {

namespace {

constexpr int kSchemaVersion = 1;

void set_error(StoreError* error, const std::string& code, const std::string& message) {
    if (!error) return;
    error->code = code;
    error->message = message;
    error->conflict.reset();
}

std::string sqlite_message(sqlite3* db, const std::string& prefix) {
    return prefix + (db ? ": " + std::string(sqlite3_errmsg(db)) : std::string{});
}

bool exec_sql(sqlite3* db, const char* sql, StoreError* error) {
    char* raw = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw);
    if (rc == SQLITE_OK) return true;
    const std::string message = raw ? raw : (db ? sqlite3_errmsg(db) : "sqlite error");
    sqlite3_free(raw);
    set_error(error, "SQLITE_ERROR", message);
    return false;
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql, StoreError* error) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            set_error(error, "SQLITE_ERROR", sqlite_message(db, "prepare failed"));
        }
    }
    ~Statement() { if (stmt_) sqlite3_finalize(stmt_); }
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

bool bind_optional_i64(sqlite3_stmt* stmt, int index,
                       const std::optional<std::int64_t>& value) {
    return value ? bind_i64(stmt, index, *value)
                 : sqlite3_bind_null(stmt, index) == SQLITE_OK;
}

std::string column_text(sqlite3_stmt* stmt, int index) {
    const unsigned char* value = sqlite3_column_text(stmt, index);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::optional<std::int64_t> column_optional_i64(sqlite3_stmt* stmt, int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) return std::nullopt;
    return static_cast<std::int64_t>(sqlite3_column_int64(stmt, index));
}

LoopDefinition loop_from_row(sqlite3_stmt* stmt, StoreError* error) {
    LoopDefinition value;
    value.id = column_text(stmt, 0);
    value.name = column_text(stmt, 1);
    value.prompt = column_text(stmt, 2);
    value.workspace_hash = column_text(stmt, 3);
    value.workspace_cwd = column_text(stmt, 4);
    value.model_name = column_text(stmt, 5);
    value.permission_mode = column_text(stmt, 6);
    const std::string schedule_json = column_text(stmt, 7);
    value.schedule_expr = column_text(stmt, 8);
    value.next_run_at_ms = column_optional_i64(stmt, 9);
    value.enabled = sqlite3_column_int(stmt, 10) != 0;
    value.created_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 11));
    value.updated_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 12));
    try {
        ValidationError validation;
        if (!schedule_from_json(nlohmann::json::parse(schedule_json), value.schedule, &validation)) {
            set_error(error, "CORRUPT_SCHEDULE", validation.message);
            value.id.clear();
        }
    } catch (const std::exception& e) {
        set_error(error, "CORRUPT_SCHEDULE", e.what());
        value.id.clear();
    }
    return value;
}

LoopRun run_from_row(sqlite3_stmt* stmt) {
    LoopRun value;
    value.id = column_text(stmt, 0);
    value.loop_id = column_text(stmt, 1);
    value.scheduled_at_ms = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 2));
    value.started_at_ms = column_optional_i64(stmt, 3);
    value.finished_at_ms = column_optional_i64(stmt, 4);
    value.status = parse_run_status(column_text(stmt, 5)).value_or(RunStatus::Failed);
    value.reason = column_text(stmt, 6);
    value.missed_count = static_cast<std::int64_t>(sqlite3_column_int64(stmt, 7));
    value.session_id = column_text(stmt, 8);
    value.worktree_path = column_text(stmt, 9);
    value.worktree_branch = column_text(stmt, 10);
    value.owner_id = column_text(stmt, 11);
    return value;
}

constexpr const char* kLoopSelectColumns =
    "id,name,prompt,workspace_hash,workspace_cwd,model_name,permission_mode,"
    "schedule_json,schedule_expr,next_run_at_ms,enabled,created_at_ms,updated_at_ms";

bool bind_loop_write(sqlite3_stmt* stmt, const LoopDefinition& value, bool include_id) {
    int i = 1;
    if (include_id && !bind_text(stmt, i++, value.id)) return false;
    return bind_text(stmt, i++, value.name) &&
           bind_text(stmt, i++, value.prompt) &&
           bind_text(stmt, i++, value.workspace_hash) &&
           bind_text(stmt, i++, value.workspace_cwd) &&
           bind_text(stmt, i++, value.model_name) &&
           bind_text(stmt, i++, value.permission_mode) &&
           bind_text(stmt, i++, schedule_to_json(value.schedule).dump()) &&
           bind_text(stmt, i++, value.schedule_expr) &&
           bind_optional_i64(stmt, i++, value.next_run_at_ms) &&
           sqlite3_bind_int(stmt, i++, value.enabled ? 1 : 0) == SQLITE_OK &&
           bind_i64(stmt, i++, value.created_at_ms) &&
           bind_i64(stmt, i++, value.updated_at_ms);
}

bool is_terminal(RunStatus status) {
    return status == RunStatus::Completed || status == RunStatus::Failed ||
           status == RunStatus::Missed;
}

std::string workspace_key(const LoopDefinition& value) {
    if (!value.workspace_hash.empty()) return "hash:" + value.workspace_hash;
    if (!value.workspace_cwd.empty()) return "cwd:" + value.workspace_cwd;
    return {};
}

bool same_workspace_active(sqlite3* db,
                           const LoopDefinition& loop,
                           StoreError* error) {
    if (workspace_key(loop).empty()) return false;
    Statement stmt(db,
        "SELECT 1 FROM loop_runs r JOIN loops l ON l.id=r.loop_id "
        "WHERE r.status IN ('running','waiting_user') "
        "AND ((?1<>'' AND l.workspace_hash=?1) OR (?1='' AND l.workspace_cwd=?2)) "
        "LIMIT 1;", error);
    if (!stmt || !bind_text(stmt.get(), 1, loop.workspace_hash) ||
        !bind_text(stmt.get(), 2, loop.workspace_cwd)) return false;
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool insert_run(sqlite3* db, const LoopRun& run, StoreError* error) {
    Statement stmt(db,
        "INSERT INTO loop_runs(id,loop_id,scheduled_at_ms,started_at_ms,finished_at_ms,"
        "status,reason,missed_count,session_id,worktree_path,worktree_branch,owner_id) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);", error);
    if (!stmt) return false;
    const bool bound = bind_text(stmt.get(), 1, run.id) &&
        bind_text(stmt.get(), 2, run.loop_id) &&
        bind_i64(stmt.get(), 3, run.scheduled_at_ms) &&
        bind_optional_i64(stmt.get(), 4, run.started_at_ms) &&
        bind_optional_i64(stmt.get(), 5, run.finished_at_ms) &&
        bind_text(stmt.get(), 6, to_string(run.status)) &&
        bind_text(stmt.get(), 7, run.reason) &&
        bind_i64(stmt.get(), 8, run.missed_count) &&
        bind_text(stmt.get(), 9, run.session_id) &&
        bind_text(stmt.get(), 10, run.worktree_path) &&
        bind_text(stmt.get(), 11, run.worktree_branch) &&
        bind_text(stmt.get(), 12, run.owner_id);
    if (!bound || sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db, "insert run failed"));
        return false;
    }
    return true;
}

bool update_next_run(sqlite3* db,
                     const std::string& loop_id,
                     const std::optional<std::int64_t>& next,
                     std::int64_t now_ms,
                     StoreError* error) {
    Statement stmt(db,
        "UPDATE loops SET next_run_at_ms=?,updated_at_ms=? WHERE id=?;", error);
    if (!stmt || !bind_optional_i64(stmt.get(), 1, next) ||
        !bind_i64(stmt.get(), 2, now_ms) || !bind_text(stmt.get(), 3, loop_id) ||
        sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db, "advance LOOP failed"));
        return false;
    }
    return true;
}

} // namespace

LoopStore::LoopStore(std::filesystem::path database_path)
    : db_path_(std::move(database_path)) {}

LoopStore::~LoopStore() {
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) sqlite3_close(db_);
}

std::filesystem::path LoopStore::default_database_path() {
    return path_from_utf8(get_acecode_dir()) / "scheduled-loops.sqlite3";
}

bool LoopStore::initialize(StoreError* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (db_) return true;
    std::error_code ec;
    std::filesystem::create_directories(db_path_.parent_path(), ec);
    if (ec) {
        set_error(error, "IO_ERROR", "failed to create LOOP database directory: " + ec.message());
        return false;
    }
    const std::string path = path_to_utf8(db_path_);
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "open LOOP database failed"));
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    const char* schema =
        "PRAGMA foreign_keys=ON;"
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS loop_schema_migrations("
        "version INTEGER PRIMARY KEY,applied_at_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS loops("
        "id TEXT PRIMARY KEY,name TEXT NOT NULL,prompt TEXT NOT NULL,"
        "workspace_hash TEXT NOT NULL DEFAULT '',workspace_cwd TEXT NOT NULL DEFAULT '',"
        "model_name TEXT NOT NULL,permission_mode TEXT NOT NULL,"
        "schedule_json TEXT NOT NULL,schedule_expr TEXT NOT NULL,"
        "next_run_at_ms INTEGER NULL,enabled INTEGER NOT NULL DEFAULT 1,"
        "created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS loops_next_idx ON loops(enabled,next_run_at_ms);"
        "CREATE TABLE IF NOT EXISTS loop_runs("
        "id TEXT PRIMARY KEY,loop_id TEXT NOT NULL,scheduled_at_ms INTEGER NOT NULL,"
        "started_at_ms INTEGER NULL,finished_at_ms INTEGER NULL,status TEXT NOT NULL,"
        "reason TEXT NOT NULL DEFAULT '',missed_count INTEGER NOT NULL DEFAULT 1,"
        "session_id TEXT NOT NULL DEFAULT '',worktree_path TEXT NOT NULL DEFAULT '',"
        "worktree_branch TEXT NOT NULL DEFAULT '',owner_id TEXT NOT NULL DEFAULT '',"
        "UNIQUE(loop_id,scheduled_at_ms),"
        "FOREIGN KEY(loop_id) REFERENCES loops(id) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS loop_runs_loop_idx ON loop_runs(loop_id,scheduled_at_ms DESC);"
        "CREATE INDEX IF NOT EXISTS loop_runs_active_idx ON loop_runs(status,owner_id);";
    if (!exec_sql(db_, schema, error)) return false;
    Statement migration(db_,
        "INSERT OR IGNORE INTO loop_schema_migrations(version,applied_at_ms) VALUES(?,?);", error);
    if (!migration || !bind_i64(migration.get(), 1, kSchemaVersion) ||
        !bind_i64(migration.get(), 2, 0) || sqlite3_step(migration.get()) != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "record LOOP schema failed"));
        return false;
    }
    return true;
}

bool LoopStore::available() const {
    std::lock_guard<std::mutex> lock(mu_);
    return db_ != nullptr;
}

bool LoopStore::ensure_available(StoreError* error) const {
    if (db_) return true;
    set_error(error, "STORE_UNAVAILABLE", "LOOP database is not initialized");
    return false;
}

bool LoopStore::begin_locked(StoreError* error) const {
    return exec_sql(db_, "BEGIN IMMEDIATE;", error);
}

bool LoopStore::commit_locked(StoreError* error) const {
    return exec_sql(db_, "COMMIT;", error);
}

void LoopStore::rollback_locked() const {
    StoreError ignored;
    exec_sql(db_, "ROLLBACK;", &ignored);
}

std::vector<LoopDefinition> LoopStore::list_loops_locked(StoreError* error) const {
    std::vector<LoopDefinition> result;
    const std::string sql = std::string("SELECT ") + kLoopSelectColumns +
        " FROM loops ORDER BY created_at_ms DESC,id;";
    Statement stmt(db_, sql.c_str(), error);
    if (!stmt) return result;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        auto value = loop_from_row(stmt.get(), error);
        if (value.id.empty()) return {};
        result.push_back(std::move(value));
    }
    if (rc != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "list LOOP failed"));
        return {};
    }
    return result;
}

std::vector<LoopDefinition> LoopStore::list_loops(StoreError* error) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error)) return {};
    return list_loops_locked(error);
}

std::optional<LoopDefinition> LoopStore::get_loop_locked(const std::string& id,
                                                         StoreError* error) const {
    const std::string sql = std::string("SELECT ") + kLoopSelectColumns +
        " FROM loops WHERE id=?;";
    Statement stmt(db_, sql.c_str(), error);
    if (!stmt || !bind_text(stmt.get(), 1, id)) return std::nullopt;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "read LOOP failed"));
        return std::nullopt;
    }
    auto value = loop_from_row(stmt.get(), error);
    return value.id.empty() ? std::nullopt : std::optional<LoopDefinition>(std::move(value));
}

std::optional<LoopDefinition> LoopStore::get_loop(const std::string& id,
                                                  StoreError* error) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error)) return std::nullopt;
    return get_loop_locked(id, error);
}

std::optional<LoopDefinition> LoopStore::create_loop(LoopDefinition value,
                                                     std::int64_t now_ms,
                                                     StoreError* error) {
    normalize_loop_definition(value);
    ValidationError validation;
    ScheduleCompilation compiled;
    if (!validate_loop_definition(value, &validation) ||
        !compile_schedule(value.schedule, now_ms, compiled, &validation)) {
        set_error(error, validation.code, validation.message);
        return std::nullopt;
    }
    value.id = value.id.empty() ? generate_uuid() : value.id;
    value.schedule_expr = compiled.expression;
    value.next_run_at_ms = value.enabled ? compiled.next_run_at_ms : std::nullopt;
    value.created_at_ms = now_ms;
    value.updated_at_ms = now_ms;

    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error) || !begin_locked(error)) return std::nullopt;
    auto existing = list_loops_locked(error);
    if (error && !error->code.empty()) { rollback_locked(); return std::nullopt; }
    if (auto conflict = find_schedule_conflict(value, existing, now_ms)) {
        rollback_locked();
        set_error(error, "SCHEDULE_CONFLICT", "workspace schedule conflicts with another LOOP");
        if (error) error->conflict = *conflict;
        return std::nullopt;
    }
    Statement stmt(db_,
        "INSERT INTO loops(id,name,prompt,workspace_hash,workspace_cwd,model_name,"
        "permission_mode,schedule_json,schedule_expr,next_run_at_ms,enabled,created_at_ms,updated_at_ms) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);", error);
    if (!stmt || !bind_loop_write(stmt.get(), value, true) ||
        sqlite3_step(stmt.get()) != SQLITE_DONE || !commit_locked(error)) {
        if (sqlite3_get_autocommit(db_) == 0) rollback_locked();
        if (error && error->code.empty()) set_error(error, "SQLITE_ERROR", sqlite_message(db_, "create LOOP failed"));
        return std::nullopt;
    }
    return value;
}

std::optional<LoopDefinition> LoopStore::update_loop(const std::string& id,
                                                     LoopDefinition value,
                                                     std::int64_t now_ms,
                                                     StoreError* error) {
    normalize_loop_definition(value);
    ValidationError validation;
    ScheduleCompilation compiled;
    if (!validate_loop_definition(value, &validation) ||
        !compile_schedule(value.schedule, now_ms, compiled, &validation)) {
        set_error(error, validation.code, validation.message);
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error) || !begin_locked(error)) return std::nullopt;
    auto previous = get_loop_locked(id, error);
    if (!previous) {
        rollback_locked();
        if (!error || error->code.empty()) set_error(error, "NOT_FOUND", "LOOP not found");
        return std::nullopt;
    }
    value.id = id;
    value.created_at_ms = previous->created_at_ms;
    value.updated_at_ms = now_ms;
    value.schedule_expr = compiled.expression;
    value.next_run_at_ms = value.enabled ? compiled.next_run_at_ms : std::nullopt;
    auto existing = list_loops_locked(error);
    if (error && !error->code.empty()) { rollback_locked(); return std::nullopt; }
    if (auto conflict = find_schedule_conflict(value, existing, now_ms)) {
        rollback_locked();
        set_error(error, "SCHEDULE_CONFLICT", "workspace schedule conflicts with another LOOP");
        if (error) error->conflict = *conflict;
        return std::nullopt;
    }
    Statement stmt(db_,
        "UPDATE loops SET name=?,prompt=?,workspace_hash=?,workspace_cwd=?,model_name=?,"
        "permission_mode=?,schedule_json=?,schedule_expr=?,next_run_at_ms=?,enabled=?,"
        "created_at_ms=?,updated_at_ms=? WHERE id=?;", error);
    if (!stmt || !bind_loop_write(stmt.get(), value, false) ||
        !bind_text(stmt.get(), 13, id) || sqlite3_step(stmt.get()) != SQLITE_DONE ||
        !commit_locked(error)) {
        if (sqlite3_get_autocommit(db_) == 0) rollback_locked();
        if (error && error->code.empty()) set_error(error, "SQLITE_ERROR", sqlite_message(db_, "update LOOP failed"));
        return std::nullopt;
    }
    return value;
}

std::optional<LoopDefinition> LoopStore::set_loop_enabled(const std::string& id,
                                                          bool enabled,
                                                          std::int64_t now_ms,
                                                          StoreError* error) {
    auto current = get_loop(id, error);
    if (!current) {
        if (!error || error->code.empty()) set_error(error, "NOT_FOUND", "LOOP not found");
        return std::nullopt;
    }
    current->enabled = enabled;
    return update_loop(id, *current, now_ms, error);
}

bool LoopStore::delete_loop(const std::string& id, StoreError* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error)) return false;
    Statement stmt(db_, "DELETE FROM loops WHERE id=?;", error);
    if (!stmt || !bind_text(stmt.get(), 1, id) || sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "delete LOOP failed"));
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        set_error(error, "NOT_FOUND", "LOOP not found");
        return false;
    }
    return true;
}

std::vector<LoopRun> LoopStore::list_runs(const std::string& loop_id,
                                          int limit,
                                          StoreError* error) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<LoopRun> result;
    if (!ensure_available(error)) return result;
    limit = std::clamp(limit, 1, 500);
    Statement stmt(db_,
        "SELECT id,loop_id,scheduled_at_ms,started_at_ms,finished_at_ms,status,reason,"
        "missed_count,session_id,worktree_path,worktree_branch,owner_id "
        "FROM loop_runs WHERE loop_id=? ORDER BY scheduled_at_ms DESC LIMIT ?;", error);
    if (!stmt || !bind_text(stmt.get(), 1, loop_id) ||
        sqlite3_bind_int(stmt.get(), 2, limit) != SQLITE_OK) return result;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) result.push_back(run_from_row(stmt.get()));
    if (rc != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "list LOOP runs failed"));
        return {};
    }
    return result;
}

std::optional<std::int64_t> LoopStore::earliest_next_run_at(StoreError* error) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error)) return std::nullopt;
    Statement stmt(db_,
        "SELECT MIN(next_run_at_ms) FROM loops WHERE enabled=1 AND next_run_at_ms IS NOT NULL;", error);
    if (!stmt || sqlite3_step(stmt.get()) != SQLITE_ROW) return std::nullopt;
    return column_optional_i64(stmt.get(), 0);
}

bool LoopStore::record_offline_missed(std::int64_t now_ms,
                                      const std::string& owner_id,
                                      StoreError* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error) || !begin_locked(error)) return false;
    auto loops = list_loops_locked(error);
    if (error && !error->code.empty()) { rollback_locked(); return false; }
    for (auto& loop : loops) {
        if (!loop.enabled || !loop.next_run_at_ms || *loop.next_run_at_ms >= now_ms) continue;
        auto advanced = advance_missed_occurrences(loop.schedule, *loop.next_run_at_ms, now_ms);
        if (advanced.missed_count <= 0 || !advanced.last_missed_at_ms) continue;
        LoopRun run;
        run.id = generate_uuid();
        run.loop_id = loop.id;
        run.scheduled_at_ms = *advanced.last_missed_at_ms;
        run.finished_at_ms = now_ms;
        run.status = RunStatus::Missed;
        run.reason = "daemon_offline";
        run.missed_count = advanced.missed_count;
        run.owner_id = owner_id;
        if (!insert_run(db_, run, error) ||
            !update_next_run(db_, loop.id, advanced.next_run_at_ms, now_ms, error)) {
            rollback_locked();
            return false;
        }
    }
    return commit_locked(error);
}

ClaimResult LoopStore::claim_due(std::int64_t now_ms,
                                 const std::string& owner_id,
                                 StoreError* error) {
    std::lock_guard<std::mutex> lock(mu_);
    ClaimResult result;
    if (!ensure_available(error) || !begin_locked(error)) return result;
    const std::string sql = std::string("SELECT ") + kLoopSelectColumns +
        " FROM loops WHERE enabled=1 AND next_run_at_ms IS NOT NULL "
        "AND next_run_at_ms<=? ORDER BY next_run_at_ms,id LIMIT 1;";
    Statement stmt(db_, sql.c_str(), error);
    if (!stmt || !bind_i64(stmt.get(), 1, now_ms)) { rollback_locked(); return result; }
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        if (!commit_locked(error)) rollback_locked();
        return result;
    }
    if (rc != SQLITE_ROW) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "select due LOOP failed"));
        rollback_locked();
        return result;
    }
    LoopDefinition loop = loop_from_row(stmt.get(), error);
    if (loop.id.empty() || !loop.next_run_at_ms) { rollback_locked(); return result; }
    const std::int64_t scheduled_at = *loop.next_run_at_ms;
    const auto next = next_occurrence_ms(loop.schedule, scheduled_at);
    const bool busy = same_workspace_active(db_, loop, error);
    if (error && !error->code.empty()) { rollback_locked(); return result; }

    LoopRun run;
    run.id = generate_uuid();
    run.loop_id = loop.id;
    run.scheduled_at_ms = scheduled_at;
    run.owner_id = owner_id;
    if (busy) {
        run.finished_at_ms = now_ms;
        run.status = RunStatus::Missed;
        run.reason = "workspace_busy";
        result.disposition = ClaimDisposition::MissedWorkspaceBusy;
    } else {
        run.started_at_ms = now_ms;
        run.status = RunStatus::Running;
        result.disposition = ClaimDisposition::Claimed;
    }
    if (!insert_run(db_, run, error) ||
        !update_next_run(db_, loop.id, next, now_ms, error) ||
        !commit_locked(error)) {
        if (sqlite3_get_autocommit(db_) == 0) rollback_locked();
        result = {};
        return result;
    }
    loop.next_run_at_ms = next;
    result.loop = std::move(loop);
    result.run = std::move(run);
    return result;
}

bool LoopStore::update_run_state(const std::string& run_id,
                                 RunStatus status,
                                 std::int64_t now_ms,
                                 const std::string& reason,
                                 const std::string& session_id,
                                 const std::string& worktree_path,
                                 const std::string& worktree_branch,
                                 StoreError* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error)) return false;
    Statement stmt(db_,
        "UPDATE loop_runs SET status=?,reason=?,"
        "finished_at_ms=CASE WHEN ? THEN ? ELSE NULL END,"
        "session_id=CASE WHEN ?<>'' THEN ? ELSE session_id END,"
        "worktree_path=CASE WHEN ?<>'' THEN ? ELSE worktree_path END,"
        "worktree_branch=CASE WHEN ?<>'' THEN ? ELSE worktree_branch END "
        "WHERE id=?;", error);
    if (!stmt) return false;
    const bool terminal = is_terminal(status);
    const bool bound = bind_text(stmt.get(), 1, to_string(status)) &&
        bind_text(stmt.get(), 2, reason) &&
        sqlite3_bind_int(stmt.get(), 3, terminal ? 1 : 0) == SQLITE_OK &&
        bind_i64(stmt.get(), 4, now_ms) &&
        bind_text(stmt.get(), 5, session_id) && bind_text(stmt.get(), 6, session_id) &&
        bind_text(stmt.get(), 7, worktree_path) && bind_text(stmt.get(), 8, worktree_path) &&
        bind_text(stmt.get(), 9, worktree_branch) && bind_text(stmt.get(), 10, worktree_branch) &&
        bind_text(stmt.get(), 11, run_id);
    if (!bound || sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "update LOOP run failed"));
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        set_error(error, "NOT_FOUND", "LOOP run not found");
        return false;
    }
    return true;
}

bool LoopStore::interrupt_owner_runs(const std::string& owner_id,
                                     std::int64_t now_ms,
                                     StoreError* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ensure_available(error)) return false;
    Statement stmt(db_,
        "UPDATE loop_runs SET status='failed',reason='daemon_interrupted',finished_at_ms=? "
        "WHERE owner_id=? AND status IN ('running','waiting_user');", error);
    if (!stmt || !bind_i64(stmt.get(), 1, now_ms) || !bind_text(stmt.get(), 2, owner_id) ||
        sqlite3_step(stmt.get()) != SQLITE_DONE) {
        set_error(error, "SQLITE_ERROR", sqlite_message(db_, "interrupt LOOP runs failed"));
        return false;
    }
    return true;
}

} // namespace acecode::loop
