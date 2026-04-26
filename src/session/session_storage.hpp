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
    // 同一 session_id 在磁盘上可能存在多个 pid 后缀文件（daemon + TUI 并发场景）；
    // 此函数按 id 去重，每个 id 只返回 mtime 最新那份对应的 SessionMeta。
    static std::vector<SessionMeta> list_sessions(const std::string& project_dir);

    // 一份 session 文件的候选记录，用于 resume 时做"多 pid 文件取最新"决策。
    struct SessionFileCandidate {
        std::string jsonl_path;
        std::string meta_path;
        int pid = 0;            // 0 = 旧格式无 pid 后缀
        std::int64_t mtime = 0; // file_clock::now epoch seconds，用于排序
    };

    // 在 project_dir 下查找所有匹配 `<session_id>(-<pid>)?.jsonl` 的文件。
    // 返回结果按 mtime 降序（最近的在前）。空表示该 id 在磁盘上不存在。
    static std::vector<SessionFileCandidate> find_session_files(
        const std::string& project_dir, const std::string& session_id);

    // Get the JSONL file path for a session.
    // pid == -1（默认）→ 自动用本进程 pid，结果 `<dir>/<id>-<pid>.jsonl`
    // pid == 0          → 旧格式，无 pid 后缀，结果 `<dir>/<id>.jsonl`（兼容读取用）
    // pid >  0          → 显式指定，结果 `<dir>/<id>-<pid>.jsonl`
    static std::string session_path(const std::string& project_dir,
                                    const std::string& session_id,
                                    int pid = -1);

    // Get the meta file path for a session. pid 语义同 session_path。
    static std::string meta_path(const std::string& project_dir,
                                 const std::string& session_id,
                                 int pid = -1);

    // Get current time as ISO 8601 string (UTC)
    static std::string now_iso8601();
};

} // namespace acecode
