#pragma once

#include "file_checkpoint_store.hpp"
#include "compact_checkpoint.hpp"
#include "session_storage.hpp"
#include "session_writer_lease.hpp"
#include "thread_goal_store.hpp"
#include "../provider/llm_provider.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <memory>
#include <mutex>

namespace acecode {

class SessionManager {
public:
    // Prepare a new session (lazy: files created on first message)
    void start_session(const std::string& cwd,
                       const std::string& provider,
                       const std::string& model,
                       const std::string& preset_session_id = "",
                       const std::string& model_preset = "",
                       const std::string& surface = "tui",
                       bool no_workspace = false);

    // Called for each message produced during conversation.
    // Appends to JSONL and periodically updates metadata.
    void on_message(const ChatMessage& msg);

    // Replace the current active JSONL transcript, preserving checkpoint
    // metadata for user turns that remain in the supplied message list.
    bool replace_active_messages(const std::vector<ChatMessage>& messages);

    // Append a compact checkpoint to the current JSONL transcript without
    // rewriting older human-visible rows.
    bool append_compact_checkpoint(const CompactCheckpoint& checkpoint);

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

    // Read the active session transcript without mutating session state.
    // Returns empty when no active JSONL has been created yet.
    std::vector<ChatMessage> load_active_messages() const;

    // Read the SessionMeta for a previously persisted session by ID, without
    // mutating any in-memory state. Returns empty SessionMeta (id == "") when
    // the meta file is missing. Used by main.cpp's resume path so it can apply
    // the persisted provider/model to the runtime LlmProvider before the
    // session is re-activated. (openspec model-profiles task 6.1.)
    SessionMeta load_session_meta(const std::string& session_id) const;

    // True when the current project has a canonical transcript for session_id.
    bool has_session_file(const std::string& session_id) const;

    // True when the current project contains incompatible old PID-suffixed
    // session data. Empty session_id checks for any old data in the project.
    bool has_incompatible_session_data(const std::string& session_id = "") const;

    // Last recoverable session error, such as a writer lease conflict.
    std::string last_error() const;

    // After main.cpp swaps the provider, call this so subsequent meta updates
    // record the new provider/model name. Pure setter; thread-safe.
    void set_active_provider(const std::string& provider, const std::string& model);
    void set_active_provider(const std::string& provider,
                             const std::string& model,
                             const std::string& model_preset);

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

    // Ensure a canonical session id and metadata file exist, then return the id.
    // Used by goal commands/tools, which can create state before the first chat
    // message is written.
    std::string ensure_active_session_id();

    // Directory for full tool outputs persisted out of the prompt context.
    // Creates the active session lazily, matching on_message/goal behavior.
    std::string ensure_tool_results_dir();

    // Narrow allowlist for file_read: persisted tool results live in ACECode's
    // session store, outside cwd, but the model needs to read them back.
    bool is_tool_result_artifact_path(const std::string& path) const;

    ThreadGoalStore* goal_store();
    ThreadGoalStore* existing_goal_store();
    const ThreadGoalStore* goal_store() const;

    // Set the in-memory title for the current session. Persisted to .meta.json
    // on the next update_meta() (every 5 messages, or finalize). Pass empty
    // string to clear. Explicit user titles take precedence over generated
    // titles.
    void set_session_title(std::string title);
    bool try_set_generated_session_title(std::string title);
    bool mark_auto_title_generation_started();

    // Set the in-memory archive state for the current session and persist it
    // immediately when metadata already exists.
    void set_session_archived(bool archived);

    // Mark the current session as a spawn_subagent child of parent_id.
    // Persisted to .meta.json (immediately when metadata already exists,
    // otherwise on lazy creation). Pass empty string to clear.
    void set_parent_session_id(std::string parent_id);
    std::string current_parent_session_id() const;

    // Return the current in-memory title (empty when unset).
    std::string current_title() const;
    std::string current_title_source() const;

    // Persisted unsubmitted chat input draft for the active session.
    void set_input_draft(std::string draft);
    std::string current_input_draft() const;

    // Persisted runtime state for the active session.
    void set_permission_mode(std::string mode, bool persist_immediately = true);
    std::string current_permission_mode() const;
    void set_pre_plan_permission_mode(std::string mode, bool persist_immediately = true);
    std::string current_pre_plan_permission_mode() const;
    void set_todos(std::vector<TodoItem> todos, bool persist_immediately = true);
    std::vector<TodoItem> current_todos() const;
    std::string ensure_plan_file_path();
    std::string current_plan_file_path() const;
    std::string read_plan_file() const;
    bool write_plan_file(const std::string& content, std::string* error = nullptr);
    bool is_plan_file_path(const std::string& path) const;
    void record_token_usage(const TokenUsage& usage);
    TokenUsage current_last_token_usage() const;
    TokenUsage current_session_token_usage() const;
    int current_turn_count() const;

private:
    bool ensure_created();  // Lazy creation of session files on first message
    void update_meta();     // Write current metadata to disk
    std::string extract_summary(const std::string& content) const;
    bool acquire_writer_lease_locked();
    void refresh_writer_lease_locked();
    void release_writer_lease_locked();

    std::string cwd_;
    std::string provider_name_;
    std::string model_name_;
    std::string model_preset_;
    std::string surface_ = "tui";
    bool no_workspace_ = false;
    std::string project_dir_;
    std::string session_id_;
    std::string jsonl_path_;
    std::string meta_path_str_;

    bool started_ = false;    // start_session() called
    bool created_ = false;    // Files actually created (lazy)
    bool finalized_ = false;  // finalize() called

    int message_count_ = 0;
    int turn_count_ = 0;
    std::string last_user_summary_;
    std::string created_at_;
    std::string pending_title_;
    std::string title_source_;
    bool auto_title_generation_attempted_ = false;
    bool user_title_touched_ = false;
    std::string input_draft_;
    std::string permission_mode_ = "default";
    std::string pre_plan_permission_mode_;
    TokenUsage last_token_usage_;
    TokenUsage session_token_usage_;
    std::vector<TodoItem> todos_;
    std::string last_error_;
    bool writer_lease_active_ = false;
    bool archived_ = false;
    std::string parent_session_id_;
    FileCheckpointStore checkpoint_store_;
    std::unique_ptr<ThreadGoalStore> goal_store_;

    mutable std::mutex mu_;
};

} // namespace acecode
