#pragma once

#include "file_checkpoint_store.hpp"
#include "session_storage.hpp"
#include "../provider/llm_provider.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <mutex>

namespace acecode {

class SessionManager {
public:
    // Prepare a new session (lazy: files created on first message)
    void start_session(const std::string& cwd,
                       const std::string& provider,
                       const std::string& model,
                       const std::string& preset_session_id = "");

    // Called for each message produced during conversation.
    // Appends to JSONL and periodically updates metadata.
    void on_message(const ChatMessage& msg);

    // File checkpoint integration for /rewind. begin_user_turn_checkpoint()
    // creates the per-user snapshot; track_file_write_before() updates that
    // snapshot immediately before a write tool mutates a file.
    void begin_user_turn_checkpoint(const std::string& user_message_uuid);
    void track_file_write_before(const std::string& file_path);
    bool file_checkpoint_can_restore(const std::string& user_message_uuid) const;
    FileCheckpointDiffStats file_checkpoint_diff_stats(const std::string& user_message_uuid) const;
    FileCheckpointRestoreResult rewind_files_to_checkpoint(const std::string& user_message_uuid) const;

    // Finalize current session: flush and write final metadata. Safe to call multiple times.
    void finalize();

    // Resume a previous session by ID. Returns loaded messages.
    // Reopens the JSONL file for continued append.
    std::vector<ChatMessage> resume_session(const std::string& session_id);

    // Read the SessionMeta for a previously persisted session by ID, without
    // mutating any in-memory state. Returns empty SessionMeta (id == "") when
    // the meta file is missing. Used by main.cpp's resume path so it can apply
    // the persisted provider/model to the runtime LlmProvider before the
    // session is re-activated. (openspec model-profiles task 6.1.)
    SessionMeta load_session_meta(const std::string& session_id) const;

    // After main.cpp swaps the provider, call this so subsequent meta updates
    // record the new provider/model name. Pure setter; thread-safe.
    void set_active_provider(const std::string& provider, const std::string& model);

    // End current session (mark it done) so next on_message starts a new one.
    void end_current_session();

    // Fork the active session into a fresh session id containing retained_prefix
    // plus retained checkpoint metadata. The previous full transcript remains
    // untouched on disk. Used by /rewind (TUI) and POST /api/sessions/:id/fork
    // (web; with title/forked_from/fork_message_id non-empty).
    //
    // 这个版本会把 manager 切到新 session,后续 on_message 写新 jsonl;
    // 老文件保持只读。
    std::string fork_active_session(const std::vector<ChatMessage>& retained_prefix);

    // 写一个全新 session 到磁盘(JSONL + meta),不动当前 active session 状态。
    // 用于 web POST /api/sessions/:id/fork:fork 操作完成后,源 session 仍然
    // 是 manager 的 active session,新 session 是磁盘上独立文件,后续由调用方
    // 通过 SessionRegistry 装载 + 注册另一个 SessionManager。
    //
    // 失败(IO 异常)时会清理半个文件,返回空字符串。
    // file_checkpoint 元消息(is_meta + subtype="file_checkpoint")自动过滤,
    // 新 session 不继承 checkpoint(spec 的明确决定)。
    std::string fork_session_to_new_id(
        const std::vector<ChatMessage>& retained_prefix,
        const std::string& title,
        const std::string& forked_from_id,
        const std::string& fork_message_id);

    // Cleanup old sessions beyond max_sessions limit.
    void cleanup_old_sessions(int max_sessions);

    // List sessions for the current project
    std::vector<SessionMeta> list_sessions() const;

    // Get current session ID (empty if no active session)
    std::string current_session_id() const;

    bool has_active_session() const;

    // Set the in-memory title for the current session. Persisted to .meta.json
    // on the next update_meta() (every 5 messages, or finalize). Pass empty
    // string to clear.
    void set_session_title(std::string title);

    // Return the current in-memory title (empty when unset).
    std::string current_title() const;

private:
    void ensure_created();  // Lazy creation of session files on first message
    void update_meta();     // Write current metadata to disk
    std::string extract_summary(const std::string& content) const;

    std::string cwd_;
    std::string provider_name_;
    std::string model_name_;
    std::string project_dir_;
    std::string session_id_;
    std::string jsonl_path_;
    std::string meta_path_str_;

    bool started_ = false;    // start_session() called
    bool created_ = false;    // Files actually created (lazy)
    bool finalized_ = false;  // finalize() called

    int message_count_ = 0;
    std::string last_user_summary_;
    std::string created_at_;
    std::string pending_title_;
    FileCheckpointStore checkpoint_store_;

    mutable std::mutex mu_;
};

} // namespace acecode
