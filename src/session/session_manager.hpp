#pragma once

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
    void start_session(const std::string& cwd, const std::string& provider, const std::string& model);

    // Called for each message produced during conversation.
    // Appends to JSONL and periodically updates metadata.
    void on_message(const ChatMessage& msg);

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

    mutable std::mutex mu_;
};

} // namespace acecode
