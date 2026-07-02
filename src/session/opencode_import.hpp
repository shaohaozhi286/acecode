#pragma once

#include "../provider/llm_provider.hpp"
#include "session_storage.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace acecode {

struct OpencodeSourceSession {
    std::string source_database;
    std::string source_identity;
    std::string opencode_session_id;
    std::string directory;
    std::string title;
    std::string provider;
    std::string model;
    std::int64_t time_created_ms = 0;
    std::int64_t time_updated_ms = 0;
    std::int64_t time_archived_ms = 0;
    bool archived = false;
    int message_count = 0;
    int part_count = 0;
};

struct OpencodeImportPreview {
    bool available = false;
    int count = 0;
    std::string source_database;
    std::string error;
    std::vector<OpencodeSourceSession> sessions;
};

struct OpencodeImportSessionResult {
    bool imported = false;
    bool skipped = false;
    std::string opencode_session_id;
    std::string acecode_session_id;
    std::string title;
    std::string error;
};

struct OpencodeImportJobStatus {
    std::string job_id;
    std::string workspace_hash;
    std::string state = "pending"; // pending | running | complete | failed
    int imported = 0;
    int total = 0;
    int failed = 0;
    int skipped = 0;
    std::string current_title;
    std::string error;
    std::vector<std::string> session_ids;
};

struct OpencodeImportOptions {
    std::string workspace_hash;
    std::string cwd;
    std::string project_dir;
    std::string source_database_override;
    bool selected_session_ids_provided = false;
    std::vector<std::string> selected_session_ids;
};

using OpencodeImportProgress = std::function<void(const OpencodeImportJobStatus&)>;

std::vector<std::string> discover_opencode_database_candidates();

OpencodeImportPreview preview_opencode_import(const std::string& cwd,
                                              const std::string& project_dir);

OpencodeImportPreview preview_opencode_import_from_database(
    const std::string& database_path,
    const std::string& cwd,
    const std::string& project_dir);

OpencodeImportJobStatus import_opencode_sessions(
    const OpencodeImportOptions& options,
    const OpencodeImportProgress& progress = {});

OpencodeImportJobStatus import_opencode_sessions_from_database(
    const std::string& database_path,
    const OpencodeImportOptions& options,
    const OpencodeImportProgress& progress = {});

std::vector<ChatMessage> convert_opencode_session_messages_for_test(
    const std::string& database_path,
    const OpencodeSourceSession& source);

} // namespace acecode
