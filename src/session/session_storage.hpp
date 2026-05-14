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
    std::string model_preset;  // optional saved_models name for this session
    std::string title;  // optional user-set window title; empty = unset

    // Web fork 相关元数据(openspec session-fork capability)。
    // 空字符串 = 这个 session 不是从其它 session 分叉出来的。
    // 老 meta 文件没有这两个字段时读出来就是空,序列化时也省略。
    std::string forked_from;       // 源 session id
    std::string fork_message_id;   // 在源 session 哪条消息上分叉(含此条)

    // Sidebar archive state. Missing field in legacy metadata is false.
    bool archived = false;
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

    // Rewrite a session JSONL file with all messages using one stream.
    static void write_messages(const std::string& session_path,
                               const std::vector<ChatMessage>& messages);

    // Load all messages from a JSONL session file.
    // Skips unparseable trailing lines (crash protection).
    static std::vector<ChatMessage> load_messages(const std::string& session_path);

    // Write session metadata to a .meta.json file.
    static void write_meta(const std::string& meta_path, const SessionMeta& meta);

    // Read session metadata from a .meta.json file.
    static SessionMeta read_meta(const std::string& meta_path);

    // List canonical sessions in a project directory, sorted by updated_at descending.
    // PID-suffixed files are incompatible old data and are ignored.
    static std::vector<SessionMeta> list_sessions(const std::string& project_dir);

    // Canonical session file record used by resume/web history paths.
    struct SessionFileCandidate {
        std::string jsonl_path;
        std::string meta_path;
        int pid = 0;            // Always 0 for canonical files.
        std::int64_t mtime = 0; // file_clock tick count, used for sorting if needed.
    };

    // Find the canonical `<session_id>.jsonl` file in project_dir.
    // PID-suffixed files are incompatible old data and are not returned.
    static std::vector<SessionFileCandidate> find_session_files(
        const std::string& project_dir, const std::string& session_id);

    // Detect incompatible old `<session-id>-<pid>.jsonl` or `.meta.json` data.
    // If session_id is empty, checks whether any such old data exists in project_dir.
    static bool has_incompatible_pid_session_files(
        const std::string& project_dir, const std::string& session_id = "");

    // Get the JSONL file path for a session.
    // Default and pid <= 0 return canonical `<dir>/<id>.jsonl`.
    // pid > 0 returns an old PID-suffixed path for tests/diagnostics only.
    static std::string session_path(const std::string& project_dir,
                                    const std::string& session_id,
                                    int pid = 0);

    // Get the meta file path for a session. pid semantics match session_path.
    static std::string meta_path(const std::string& project_dir,
                                 const std::string& session_id,
                                 int pid = 0);

    // Get current time as ISO 8601 string (UTC)
    static std::string now_iso8601();
};

} // namespace acecode
