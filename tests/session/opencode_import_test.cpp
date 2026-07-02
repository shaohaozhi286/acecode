#include <gtest/gtest.h>

#include "session/opencode_import.hpp"
#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_opencode_import_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void exec_sql(sqlite3* db, const char* sql) {
    char* raw_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw_error);
    std::string error = raw_error ? raw_error : "";
    sqlite3_free(raw_error);
    ASSERT_EQ(rc, SQLITE_OK) << error;
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    ASSERT_EQ(sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
}

void bind_i64(sqlite3_stmt* stmt, int index, std::int64_t value) {
    ASSERT_EQ(sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value)), SQLITE_OK);
}

void run_insert(sqlite3* db, const char* sql, const std::vector<std::string>& text_values) {
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr), SQLITE_OK);
    for (std::size_t i = 0; i < text_values.size(); ++i) {
        bind_text(stmt, static_cast<int>(i + 1), text_values[i]);
    }
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

void create_schema(sqlite3* db) {
    exec_sql(db,
        "CREATE TABLE project ("
        "id TEXT PRIMARY KEY,"
        "worktree TEXT"
        ");"
        "CREATE TABLE session ("
        "id TEXT PRIMARY KEY,"
        "project_id TEXT,"
        "directory TEXT,"
        "title TEXT,"
        "time_created INTEGER,"
        "time_updated INTEGER,"
        "time_archived INTEGER,"
        "model TEXT"
        ");"
        "CREATE TABLE message ("
        "id TEXT PRIMARY KEY,"
        "session_id TEXT,"
        "time_created INTEGER,"
        "time_updated INTEGER,"
        "data TEXT"
        ");"
        "CREATE TABLE part ("
        "id TEXT PRIMARY KEY,"
        "message_id TEXT,"
        "session_id TEXT,"
        "time_created INTEGER,"
        "time_updated INTEGER,"
        "data TEXT"
        ");");
    run_insert(db, "INSERT INTO project(id, worktree) VALUES(?, ?);", {"proj", ""});
}

void insert_session(sqlite3* db,
                    const std::string& id,
                    const std::string& directory,
                    const std::string& title = "Imported title",
                    std::int64_t archived_ms = 0) {
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO session(id, project_id, directory, title, time_created, time_updated, time_archived, model) "
        "VALUES(?, 'proj', ?, ?, ?, ?, ?, ?);",
        -1, &stmt, nullptr), SQLITE_OK);
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, directory);
    bind_text(stmt, 3, title);
    bind_i64(stmt, 4, 1700000000000);
    bind_i64(stmt, 5, 1700000010000);
    bind_i64(stmt, 6, archived_ms);
    bind_text(stmt, 7, R"({"providerID":"opencode","modelID":"big-pickle"})");
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

void insert_message(sqlite3* db,
                    const std::string& session_id,
                    const std::string& id,
                    const std::string& data,
                    std::int64_t time_ms = 1700000000000) {
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO message(id, session_id, time_created, time_updated, data) "
        "VALUES(?, ?, ?, ?, ?);",
        -1, &stmt, nullptr), SQLITE_OK);
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, session_id);
    bind_i64(stmt, 3, time_ms);
    bind_i64(stmt, 4, time_ms);
    bind_text(stmt, 5, data);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

void insert_part(sqlite3* db,
                 const std::string& session_id,
                 const std::string& message_id,
                 const std::string& id,
                 const std::string& data,
                 std::int64_t time_ms = 1700000000000) {
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO part(id, message_id, session_id, time_created, time_updated, data) "
        "VALUES(?, ?, ?, ?, ?, ?);",
        -1, &stmt, nullptr), SQLITE_OK);
    bind_text(stmt, 1, id);
    bind_text(stmt, 2, message_id);
    bind_text(stmt, 3, session_id);
    bind_i64(stmt, 4, time_ms);
    bind_i64(stmt, 5, time_ms);
    bind_text(stmt, 6, data);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
}

fs::path create_db(const fs::path& dir) {
    fs::create_directories(dir);
    fs::path db_path = dir / "opencode.db";
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(acecode::path_to_utf8(db_path).c_str(),
                              &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              nullptr), SQLITE_OK);
    create_schema(db);
    sqlite3_close(db);
    return db_path;
}

sqlite3* open_db_rw(const fs::path& db_path) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open_v2(acecode::path_to_utf8(db_path).c_str(),
                              &db,
                              SQLITE_OPEN_READWRITE,
                              nullptr), SQLITE_OK);
    return db;
}

void set_env_value(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void clear_env_value(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct EnvSnapshot {
    const char* name;
    bool had_value;
    std::string value;
};

EnvSnapshot capture_env(const char* name) {
    const char* value = std::getenv(name);
    return EnvSnapshot{name, value != nullptr, value ? std::string(value) : std::string{}};
}

void restore_env(const EnvSnapshot& snapshot) {
    if (snapshot.had_value) set_env_value(snapshot.name, snapshot.value);
    else clear_env_value(snapshot.name);
}

} // namespace

TEST(OpencodeImport, ReportsUnsupportedSchema) {
    auto root = temp_dir("unsupported");
    auto db_path = root / "opencode.db";
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(acecode::path_to_utf8(db_path).c_str(),
                              &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              nullptr), SQLITE_OK);
    sqlite3_close(db);

    auto preview = acecode::preview_opencode_import_from_database(
        acecode::path_to_utf8(db_path),
        acecode::path_to_utf8(root / "workspace"),
        acecode::path_to_utf8(root / "project"));

    EXPECT_FALSE(preview.available);
    EXPECT_EQ(preview.count, 0);
    EXPECT_NE(preview.error.find("missing table project"), std::string::npos);
}

TEST(OpencodeImport, DiscoversXdgDataCandidate) {
    auto root = temp_dir("discovery");
    auto xdg = root / "xdg";
    auto db_path = create_db(xdg / "opencode");

    auto opencode_db = capture_env("OPENCODE_DB");
    auto xdg_data = capture_env("XDG_DATA_HOME");
    clear_env_value("OPENCODE_DB");
    set_env_value("XDG_DATA_HOME", acecode::path_to_utf8(xdg));

    auto candidates = acecode::discover_opencode_database_candidates();

    restore_env(opencode_db);
    restore_env(xdg_data);

    const std::string expected = acecode::path_to_utf8(db_path);
    EXPECT_NE(std::find(candidates.begin(), candidates.end(), expected), candidates.end());
}

TEST(OpencodeImport, FiltersWorkspacePathsAndImportsOnce) {
    auto root = temp_dir("filter");
    auto workspace = root / "workspace";
    auto child = workspace / "child";
    auto project_dir = root / "ace-project";
    fs::create_directories(child);
    fs::create_directories(project_dir);
    auto db_path = create_db(root / "data");

    sqlite3* db = open_db_rw(db_path);
    ASSERT_NE(db, nullptr);
    insert_session(db, "ses-root", acecode::path_to_utf8(workspace), "Root");
    insert_message(db, "ses-root", "msg-root", R"({"role":"user"})");
    insert_part(db, "ses-root", "msg-root", "prt-root", R"({"type":"text","text":"hello"})");
    insert_session(db, "ses-child", acecode::path_to_utf8(child), "Child");
    insert_message(db, "ses-child", "msg-child", R"({"role":"user"})");
    insert_part(db, "ses-child", "msg-child", "prt-child", R"({"type":"text","text":"child"})");
    insert_session(db, "ses-other", acecode::path_to_utf8(root / "other"), "Other");
    insert_message(db, "ses-other", "msg-other", R"({"role":"user"})");
    insert_part(db, "ses-other", "msg-other", "prt-other", R"({"type":"text","text":"other"})");
    sqlite3_close(db);

    auto preview = acecode::preview_opencode_import_from_database(
        acecode::path_to_utf8(db_path),
        acecode::path_to_utf8(workspace),
        acecode::path_to_utf8(project_dir));
    ASSERT_TRUE(preview.available) << preview.error;
    EXPECT_EQ(preview.count, 2);

    acecode::OpencodeImportOptions options;
    options.workspace_hash = "hash";
    options.cwd = acecode::path_to_utf8(workspace);
    options.project_dir = acecode::path_to_utf8(project_dir);
    auto status = acecode::import_opencode_sessions_from_database(
        acecode::path_to_utf8(db_path), options);
    EXPECT_EQ(status.state, "complete");
    EXPECT_EQ(status.imported, 2);
    ASSERT_EQ(status.session_ids.size(), 2u);
    EXPECT_TRUE(fs::exists(project_dir / (status.session_ids[0] + ".jsonl")));
    EXPECT_TRUE(fs::exists(project_dir / "opencode_imports.json"));

    auto repeated = acecode::preview_opencode_import_from_database(
        acecode::path_to_utf8(db_path),
        acecode::path_to_utf8(workspace),
        acecode::path_to_utf8(project_dir));
    EXPECT_FALSE(repeated.available);
    EXPECT_EQ(repeated.count, 0);
}

TEST(OpencodeImport, ReportsArchivedStateAndImportsSelectedSessions) {
    auto root = temp_dir("select");
    auto workspace = root / "workspace";
    auto project_dir = root / "ace-project";
    fs::create_directories(workspace);
    fs::create_directories(project_dir);
    auto db_path = create_db(root / "data");

    sqlite3* db = open_db_rw(db_path);
    ASSERT_NE(db, nullptr);
    insert_session(db, "ses-active", acecode::path_to_utf8(workspace), "Active");
    insert_message(db, "ses-active", "msg-active", R"({"role":"user"})");
    insert_part(db, "ses-active", "msg-active", "prt-active", R"({"type":"text","text":"active"})");
    insert_session(db, "ses-archived", acecode::path_to_utf8(workspace), "Archived", 1700000020000);
    insert_message(db, "ses-archived", "msg-archived", R"({"role":"user"})");
    insert_part(db, "ses-archived", "msg-archived", "prt-archived", R"({"type":"text","text":"archived"})");
    sqlite3_close(db);

    auto preview = acecode::preview_opencode_import_from_database(
        acecode::path_to_utf8(db_path),
        acecode::path_to_utf8(workspace),
        acecode::path_to_utf8(project_dir));
    ASSERT_TRUE(preview.available) << preview.error;
    ASSERT_EQ(preview.count, 2);
    auto archived = std::find_if(preview.sessions.begin(), preview.sessions.end(), [](const auto& session) {
        return session.opencode_session_id == "ses-archived";
    });
    ASSERT_NE(archived, preview.sessions.end());
    EXPECT_TRUE(archived->archived);

    acecode::OpencodeImportOptions options;
    options.workspace_hash = "hash";
    options.cwd = acecode::path_to_utf8(workspace);
    options.project_dir = acecode::path_to_utf8(project_dir);
    options.selected_session_ids_provided = true;
    options.selected_session_ids = {"ses-archived"};
    auto status = acecode::import_opencode_sessions_from_database(
        acecode::path_to_utf8(db_path), options);
    EXPECT_EQ(status.state, "complete");
    EXPECT_EQ(status.total, 1);
    EXPECT_EQ(status.imported, 1);
    ASSERT_EQ(status.session_ids.size(), 1u);

    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(acecode::path_to_utf8(project_dir), status.session_ids.front()));
    EXPECT_TRUE(meta.archived);

    auto repeated = acecode::preview_opencode_import_from_database(
        acecode::path_to_utf8(db_path),
        acecode::path_to_utf8(workspace),
        acecode::path_to_utf8(project_dir));
    ASSERT_EQ(repeated.count, 1);
    EXPECT_EQ(repeated.sessions.front().opencode_session_id, "ses-active");
}

TEST(OpencodeImport, ConvertsTextReasoningToolsAndUnsupportedParts) {
    auto root = temp_dir("convert");
    auto workspace = root / "workspace";
    auto project_dir = root / "ace-project";
    fs::create_directories(workspace);
    fs::create_directories(project_dir);
    auto db_path = create_db(root / "data");

    sqlite3* db = open_db_rw(db_path);
    ASSERT_NE(db, nullptr);
    insert_session(db, "ses-convert", acecode::path_to_utf8(workspace), "Convert");
    insert_message(db, "ses-convert", "msg-user", R"({"role":"user"})");
    insert_part(db, "ses-convert", "msg-user", "prt-user", R"({"type":"text","text":"please read"})");
    insert_message(db, "ses-convert", "msg-assistant", R"({"role":"assistant","providerID":"opencode","modelID":"big-pickle"})", 1700000001000);
    insert_part(db, "ses-convert", "msg-assistant", "prt-reason", R"({"type":"reasoning","text":"thinking"})", 1700000001000);
    insert_part(db, "ses-convert", "msg-assistant", "prt-text", R"({"type":"text","text":"I will read it."})", 1700000001001);
    insert_part(db, "ses-convert", "msg-assistant", "prt-tool", R"({"type":"tool","tool":"read","callID":"call_1","state":{"status":"completed","input":{"filePath":"README.md"},"output":"contents"}})", 1700000001002);
    insert_part(db, "ses-convert", "msg-assistant", "prt-agent", R"({"type":"agent","name":"subtask"})", 1700000001003);
    sqlite3_close(db);

    auto preview = acecode::preview_opencode_import_from_database(
        acecode::path_to_utf8(db_path),
        acecode::path_to_utf8(workspace),
        acecode::path_to_utf8(project_dir));
    ASSERT_EQ(preview.count, 1);

    auto messages = acecode::convert_opencode_session_messages_for_test(
        acecode::path_to_utf8(db_path), preview.sessions.front());

    ASSERT_GE(messages.size(), 4u);
    EXPECT_EQ(messages[0].role, "user");
    EXPECT_EQ(messages[0].content, "please read");
    EXPECT_EQ(messages[1].role, "assistant");
    EXPECT_EQ(messages[1].content, "I will read it.");
    EXPECT_EQ(messages[1].reasoning_content, "thinking");
    ASSERT_TRUE(messages[1].tool_calls.is_array());
    EXPECT_EQ(messages[1].tool_calls[0]["id"], "call_1");
    EXPECT_EQ(messages[2].role, "tool");
    EXPECT_EQ(messages[2].tool_call_id, "call_1");
    EXPECT_EQ(messages[2].content, "contents");
    EXPECT_EQ(messages.back().metadata.value("transcript_only", false), true);
}
