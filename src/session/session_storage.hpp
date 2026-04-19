#pragma once

#include "../provider/llm_provider.hpp"
#include <string>
#include <vector>

namespace acecode {

struct SessionMeta {
    std::string id;
    std::string cwd;
    std::string created_at;  // ISO 8601
    std::string updated_at;  // ISO 8601
    int message_count = 0;
    std::string summary;
    std::string provider;
    std::string model;
    std::string title;  // optional user-set window title; empty = unset
};

class SessionStorage {
public:
    // Compute a project hash from working directory path.
    // Returns first 16 hex chars of a hash of the canonical path.
    static std::string compute_project_hash(const std::string& cwd);

    // Generate a new session ID: YYYYMMDD-HHMMSS-<4 hex random>
    static std::string generate_session_id();

    // Get the project directory for a given CWD: ~/.acecode/projects/<hash>/
    static std::string get_project_dir(const std::string& cwd);

    // Append a single ChatMessage as one JSONL line to a session file.
    static void append_message(const std::string& session_path, const ChatMessage& msg);

    // Load all messages from a JSONL session file.
    // Skips unparseable trailing lines (crash protection).
    static std::vector<ChatMessage> load_messages(const std::string& session_path);

    // Write session metadata to a .meta.json file.
    static void write_meta(const std::string& meta_path, const SessionMeta& meta);

    // Read session metadata from a .meta.json file.
    static SessionMeta read_meta(const std::string& meta_path);

    // List all sessions in a project directory, sorted by updated_at descending.
    static std::vector<SessionMeta> list_sessions(const std::string& project_dir);

    // Get the JSONL file path for a session: <project_dir>/<session_id>.jsonl
    static std::string session_path(const std::string& project_dir, const std::string& session_id);

    // Get the meta file path for a session: <project_dir>/<session_id>.meta.json
    static std::string meta_path(const std::string& project_dir, const std::string& session_id);

    // Get current time as ISO 8601 string (UTC)
    static std::string now_iso8601();
};

} // namespace acecode
