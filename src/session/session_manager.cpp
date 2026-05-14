#include "session_manager.hpp"
#include "session_serializer.hpp"
#include "session_rewind.hpp"
#include "../utils/logger.hpp"

#include <filesystem>
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace {

size_t utf8_safe_prefix_length(const std::string& text, size_t max_bytes) {
    const size_t limit = std::min(max_bytes, text.size());
    size_t i = 0;
    size_t last_valid = 0;

    while (i < limit) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t seq_len = 0;

        if ((c & 0x80u) == 0) {
            seq_len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            seq_len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            seq_len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            seq_len = 4;
        } else {
            break;
        }

        if (i + seq_len > limit || i + seq_len > text.size()) {
            break;
        }

        bool valid = true;
        for (size_t j = 1; j < seq_len; ++j) {
            const unsigned char continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
        }

        if (!valid) {
            break;
        }

        i += seq_len;
        last_valid = i;
    }

    return last_valid;
}

} // namespace

namespace fs = std::filesystem;

namespace acecode {

void SessionManager::start_session(const std::string& cwd,
                                   const std::string& provider,
                                   const std::string& model,
                                   const std::string& preset_session_id,
                                   const std::string& model_preset,
                                   const std::string& surface) {
    std::lock_guard<std::mutex> lk(mu_);
    release_writer_lease_locked();
    cwd_ = cwd;
    provider_name_ = provider;
    model_name_ = model;
    model_preset_ = model_preset;
    surface_ = surface.empty() ? "unknown" : surface;
    project_dir_ = SessionStorage::get_project_dir(cwd);
    session_id_ = preset_session_id;
    jsonl_path_.clear();
    meta_path_str_.clear();
    started_ = true;
    created_ = false;
    finalized_ = false;
    message_count_ = 0;
    last_user_summary_.clear();
    created_at_.clear();
    pending_title_.clear();
    last_error_.clear();
    writer_lease_active_ = false;
    archived_ = false;
    checkpoint_store_.reset();
    checkpoint_store_.set_session(project_dir_, session_id_);
}

bool SessionManager::ensure_created() {
    // Must be called under lock
    if (created_) return true;
    if (!started_) return false;
    last_error_.clear();

    // Create project directory if needed
    if (!fs::exists(project_dir_)) {
        fs::create_directories(project_dir_);
    }

    if (session_id_.empty()) {
        session_id_ = SessionStorage::generate_session_id();
    }
    jsonl_path_ = SessionStorage::session_path(project_dir_, session_id_);
    meta_path_str_ = SessionStorage::meta_path(project_dir_, session_id_);
    created_at_ = SessionStorage::now_iso8601();
    checkpoint_store_.set_session(project_dir_, session_id_);

    if (!acquire_writer_lease_locked()) {
        jsonl_path_.clear();
        meta_path_str_.clear();
        return false;
    }

    created_ = true;
    finalized_ = false;

    // Write initial metadata
    SessionMeta meta;
    meta.id = session_id_;
    meta.cwd = cwd_;
    meta.created_at = created_at_;
    meta.updated_at = created_at_;
    meta.message_count = 0;
    meta.provider = provider_name_;
    meta.model = model_name_;
    meta.model_preset = model_preset_;
    meta.title = pending_title_;
    meta.archived = archived_;
    SessionStorage::write_meta(meta_path_str_, meta);
    return true;
}

void SessionManager::on_message(const ChatMessage& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return;

    if (!ensure_created()) return;

    // Append message to JSONL
    SessionStorage::append_message(jsonl_path_, msg);
    message_count_++;

    // Track last user message for summary
    if (msg.role == "user" && !msg.content.empty()) {
        last_user_summary_ = extract_summary(msg.content);
    }

    update_meta();
}

bool SessionManager::replace_active_messages(const std::vector<ChatMessage>& messages) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return false;

    if (!ensure_created()) return false;
    if (!created_) return false;

    std::set<std::string> retained_user_uuids;
    for (const auto& msg : messages) {
        if (msg.role == "user" && !msg.uuid.empty()) {
            retained_user_uuids.insert(msg.uuid);
        }
    }

    std::unordered_map<std::string, std::vector<ChatMessage>> checkpoints_by_user;
    for (const auto& existing : SessionStorage::load_messages(jsonl_path_)) {
        auto snapshot = FileCheckpointStore::decode_snapshot_message(existing);
        if (!snapshot.has_value()) continue;
        if (retained_user_uuids.count(snapshot->message_uuid)) {
            checkpoints_by_user[snapshot->message_uuid].push_back(existing);
        }
    }

    std::vector<ChatMessage> rewritten;
    rewritten.reserve(messages.size() + checkpoints_by_user.size());
    last_user_summary_.clear();
    for (const auto& msg : messages) {
        if (is_file_checkpoint_message(msg)) continue;
        rewritten.push_back(msg);
        if (msg.role == "user") {
            if (!msg.content.empty()) {
                last_user_summary_ = extract_summary(msg.content);
            }
            auto it = checkpoints_by_user.find(msg.uuid);
            if (it != checkpoints_by_user.end()) {
                rewritten.insert(rewritten.end(), it->second.begin(), it->second.end());
            }
        }
    }

    SessionStorage::write_messages(jsonl_path_, rewritten);
    checkpoint_store_.load_from_messages(project_dir_, session_id_, rewritten);
    message_count_ = static_cast<int>(rewritten.size());
    update_meta();
    return true;
}

void SessionManager::begin_user_turn_checkpoint(const std::string& user_message_uuid) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_ || user_message_uuid.empty()) return;
    if (!ensure_created()) return;

    FileCheckpointSnapshot snapshot = checkpoint_store_.make_snapshot(user_message_uuid);
    SessionStorage::append_message(jsonl_path_, FileCheckpointStore::encode_snapshot_message(snapshot));
    message_count_++;
    update_meta();
}

void SessionManager::track_file_write_before(const std::string& file_path) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_ || file_path.empty()) return;
    if (!ensure_created()) return;

    auto snapshot = checkpoint_store_.track_before_write(file_path);
    if (!snapshot.has_value()) return;
    SessionStorage::append_message(jsonl_path_, FileCheckpointStore::encode_snapshot_message(*snapshot));
    message_count_++;
    update_meta();
}

bool SessionManager::file_checkpoint_can_restore(const std::string& user_message_uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    return checkpoint_store_.can_restore(user_message_uuid);
}

FileCheckpointDiffStats SessionManager::file_checkpoint_diff_stats(
    const std::string& user_message_uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    return checkpoint_store_.diff_stats(user_message_uuid);
}

FileCheckpointRestoreResult SessionManager::rewind_files_to_checkpoint(
    const std::string& user_message_uuid) const {
    std::lock_guard<std::mutex> lk(mu_);
    return checkpoint_store_.rewind_to(user_message_uuid);
}

void SessionManager::finalize() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || finalized_) return;
    finalized_ = true;
    update_meta();
    release_writer_lease_locked();
}

std::vector<ChatMessage> SessionManager::resume_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    last_error_.clear();

    const auto jsonl_path = SessionStorage::session_path(project_dir_, session_id);
    const auto meta_path = SessionStorage::meta_path(project_dir_, session_id);
    if (!fs::exists(jsonl_path)) {
        if (SessionStorage::has_incompatible_pid_session_files(project_dir_, session_id)) {
            LOG_WARN("[session] resume " + session_id +
                     " ignored incompatible PID-suffixed old data; delete old project session data under " +
                     project_dir_ + " and start a new session");
        }
        return {};
    }
    auto messages = SessionStorage::load_messages(jsonl_path);

    // Resume adopts the canonical transcript directly. It must not copy history
    // into a new PID-suffixed file or rewrite the shared transcript.
    if (writer_lease_active_ && session_id_ != session_id) {
        release_writer_lease_locked();
    }
    session_id_ = session_id;
    jsonl_path_ = jsonl_path;
    meta_path_str_ = meta_path;
    if (!acquire_writer_lease_locked()) {
        created_ = false;
        finalized_ = false;
        return {};
    }
    created_ = true;
    finalized_ = false;
    archived_ = false;
    checkpoint_store_.load_from_messages(project_dir_, session_id_, messages);

    // Restore persisted display/model metadata when present.
    if (fs::exists(meta_path_str_)) {
        auto meta = SessionStorage::read_meta(meta_path_str_);
        created_at_ = meta.created_at;
        last_user_summary_ = meta.summary;
        pending_title_ = meta.title;
        archived_ = meta.archived;
        if (model_preset_.empty()) {
            model_preset_ = meta.model_preset;
        }
    }
    if (created_at_.empty()) {
        created_at_ = SessionStorage::now_iso8601();
    }

    message_count_ = static_cast<int>(messages.size());
    update_meta();

    return messages;
}

SessionMeta SessionManager::load_session_meta(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty()) return {};
    const auto meta_path = SessionStorage::meta_path(project_dir_, session_id);
    if (!fs::exists(meta_path)) {
        if (SessionStorage::has_incompatible_pid_session_files(project_dir_, session_id)) {
            LOG_WARN("[session] meta for " + session_id +
                     " not loaded: incompatible PID-suffixed old data is unsupported");
        }
        return {};
    }
    return SessionStorage::read_meta(meta_path);
}

bool SessionManager::has_session_file(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty() || session_id.empty()) return false;
    const auto candidates = SessionStorage::find_session_files(project_dir_, session_id);
    return !candidates.empty();
}

bool SessionManager::has_incompatible_session_data(const std::string& session_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty()) return false;
    return SessionStorage::has_incompatible_pid_session_files(project_dir_, session_id);
}

std::string SessionManager::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_error_;
}

void SessionManager::set_active_provider(const std::string& provider,
                                         const std::string& model) {
    set_active_provider(provider, model, std::string{});
}

void SessionManager::set_active_provider(const std::string& provider,
                                         const std::string& model,
                                         const std::string& model_preset) {
    std::lock_guard<std::mutex> lk(mu_);
    provider_name_ = provider;
    model_name_ = model;
    model_preset_ = model_preset;
    if (created_) {
        update_meta();
    }
}

void SessionManager::end_current_session() {
    std::lock_guard<std::mutex> lk(mu_);
    if (created_ && !finalized_) {
        update_meta();
    }
    release_writer_lease_locked();
    // Reset so next on_message triggers a new session
    session_id_.clear();
    jsonl_path_.clear();
    meta_path_str_.clear();
    created_ = false;
    finalized_ = false;
    message_count_ = 0;
    last_user_summary_.clear();
    created_at_.clear();
    pending_title_.clear();
    last_error_.clear();
    archived_ = false;
    checkpoint_store_.reset();
    checkpoint_store_.set_session(project_dir_, "");
    // Keep started_=true, cwd_, provider_name_, model_name_, project_dir_
}

std::string SessionManager::fork_active_session(const std::vector<ChatMessage>& retained_prefix) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return {};
    if (!ensure_created()) return {};

    std::set<std::string> retained_user_uuids;
    for (const auto& msg : retained_prefix) {
        if (msg.role == "user" && !msg.uuid.empty()) {
            retained_user_uuids.insert(msg.uuid);
        }
    }

    const std::string new_session_id = SessionStorage::generate_session_id();
    auto checkpoint_meta = checkpoint_store_.fork_to_session(new_session_id, retained_user_uuids);

    release_writer_lease_locked();

    session_id_ = new_session_id;
    jsonl_path_ = SessionStorage::session_path(project_dir_, session_id_);
    meta_path_str_ = SessionStorage::meta_path(project_dir_, session_id_);
    created_at_ = SessionStorage::now_iso8601();
    created_ = false;
    finalized_ = false;
    message_count_ = 0;
    last_user_summary_.clear();

    if (!acquire_writer_lease_locked()) {
        return {};
    }
    created_ = true;

    for (auto it = retained_prefix.rbegin(); it != retained_prefix.rend(); ++it) {
        if (it->role == "user" && !it->content.empty()) {
            last_user_summary_ = extract_summary(it->content);
            break;
        }
    }

    fs::create_directories(project_dir_);
    update_meta();

    for (const auto& msg : retained_prefix) {
        if (is_file_checkpoint_message(msg)) continue;
        SessionStorage::append_message(jsonl_path_, msg);
        message_count_++;
    }
    for (const auto& msg : checkpoint_meta) {
        SessionStorage::append_message(jsonl_path_, msg);
        message_count_++;
    }
    update_meta();
    return session_id_;
}

std::string SessionManager::fork_session_to_new_id(
    const std::vector<ChatMessage>& retained_prefix,
    const std::string& title,
    const std::string& forked_from_id,
    const std::string& fork_message_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return {};

    // ensure project_dir 存在 — 即使当前 manager 还没 ensure_created
    // (理论上 web fork 调用前 manager 已经 active,但保险)。
    if (project_dir_.empty()) {
        project_dir_ = SessionStorage::get_project_dir(cwd_);
    }
    std::error_code ec;
    fs::create_directories(project_dir_, ec);
    if (ec) return {};

    const std::string new_session_id = SessionStorage::generate_session_id();
    const std::string new_jsonl = SessionStorage::session_path(project_dir_, new_session_id);
    const std::string new_meta  = SessionStorage::meta_path(project_dir_, new_session_id);

    // 写新 jsonl(过滤 file_checkpoint 元消息;新 session 不继承 checkpoints)
    int count = 0;
    std::string last_user_summary;
    bool io_error = false;
    try {
        for (const auto& msg : retained_prefix) {
            if (is_file_checkpoint_message(msg)) continue;
            SessionStorage::append_message(new_jsonl, msg);
            count++;
            if (msg.role == "user" && !msg.content.empty()) {
                last_user_summary = extract_summary(msg.content);
            }
        }
    } catch (...) {
        io_error = true;
    }

    if (io_error) {
        fs::remove(new_jsonl, ec);
        return {};
    }

    // 写新 meta:cwd / provider / model 从当前继承;forked_from / fork_message_id
    // / title 由 caller 决定。
    SessionMeta meta;
    meta.id              = new_session_id;
    meta.cwd             = cwd_;
    meta.created_at      = SessionStorage::now_iso8601();
    meta.updated_at      = meta.created_at;
    meta.message_count   = count;
    meta.summary         = last_user_summary;
    meta.provider        = provider_name_;
    meta.model           = model_name_;
    meta.model_preset    = model_preset_;
    meta.title           = title;
    meta.forked_from     = forked_from_id;
    meta.fork_message_id = fork_message_id;
    SessionStorage::write_meta(new_meta, meta);

    return new_session_id;
}

void SessionManager::cleanup_old_sessions(int max_sessions) {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty()) return;

    auto sessions = SessionStorage::list_sessions(project_dir_);
    if (static_cast<int>(sessions.size()) <= max_sessions) return;

    // Sessions are sorted newest-first; remove canonical files from the tail.
    // PID-suffixed files are incompatible old data and are not counted here.
    for (size_t i = static_cast<size_t>(max_sessions); i < sessions.size(); ++i) {
        const std::string& id = sessions[i].id;
        std::error_code ec;
        fs::remove(SessionStorage::session_path(project_dir_, id), ec);
        fs::remove(SessionStorage::meta_path(project_dir_, id), ec);
        SessionWriterLease::remove(project_dir_, id);
        FileCheckpointStore::remove_session_backups(project_dir_, id);
    }
}

std::vector<SessionMeta> SessionManager::list_sessions() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty()) return {};
    return SessionStorage::list_sessions(project_dir_);
}

std::string SessionManager::current_session_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_id_;
}

bool SessionManager::has_active_session() const {
    std::lock_guard<std::mutex> lk(mu_);
    return created_ && !finalized_;
}

void SessionManager::update_meta() {
    // Must be called under lock
    if (!created_) return;

    SessionMeta meta;
    meta.id = session_id_;
    meta.cwd = cwd_;
    meta.created_at = created_at_;
    meta.updated_at = SessionStorage::now_iso8601();
    meta.message_count = message_count_;
    meta.summary = last_user_summary_;
    meta.provider = provider_name_;
    meta.model = model_name_;
    meta.model_preset = model_preset_;
    meta.title = pending_title_;
    meta.archived = archived_;
    SessionStorage::write_meta(meta_path_str_, meta);
    refresh_writer_lease_locked();
}

void SessionManager::set_session_title(std::string title) {
    std::lock_guard<std::mutex> lk(mu_);
    pending_title_ = std::move(title);
    if (created_) {
        update_meta();
    }
}

void SessionManager::set_session_archived(bool archived) {
    std::lock_guard<std::mutex> lk(mu_);
    archived_ = archived;
    if (created_) {
        update_meta();
    }
}

std::string SessionManager::current_title() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_title_;
}

bool SessionManager::acquire_writer_lease_locked() {
    if (project_dir_.empty() || session_id_.empty()) return true;

    auto result = SessionWriterLease::acquire(project_dir_, session_id_, cwd_, surface_);
    if (result.status == SessionWriterLeaseResult::Status::Acquired) {
        writer_lease_active_ = true;
        if (result.stale_recovered) {
            LOG_INFO("[session] recovered stale writer lease for " + session_id_);
        }
        return true;
    }

    writer_lease_active_ = false;
    if (result.status == SessionWriterLeaseResult::Status::Conflict) {
        last_error_ = "Session " + session_id_ + " is already active in another ACECode process (pid " +
                      std::to_string(result.owner.pid) + ", surface " + result.owner.surface + ").";
        LOG_WARN("[session] writer lease conflict for " + session_id_ +
                 " owner_pid=" + std::to_string(result.owner.pid) +
                 " owner_surface=" + result.owner.surface);
    } else {
        last_error_ = "Failed to acquire writer lease for session " + session_id_ +
                      (result.error.empty() ? "." : ": " + result.error);
        LOG_WARN("[session] " + last_error_);
    }
    return false;
}

void SessionManager::refresh_writer_lease_locked() {
    if (!writer_lease_active_ || project_dir_.empty() || session_id_.empty()) return;
    if (!SessionWriterLease::refresh(project_dir_, session_id_)) {
        writer_lease_active_ = false;
        LOG_WARN("[session] failed to refresh writer lease for " + session_id_);
    }
}

void SessionManager::release_writer_lease_locked() {
    if (!writer_lease_active_ || project_dir_.empty() || session_id_.empty()) return;
    SessionWriterLease::release(project_dir_, session_id_);
    writer_lease_active_ = false;
}

std::string SessionManager::extract_summary(const std::string& content) const {
    constexpr size_t max_summary_bytes = 80;
    constexpr size_t min_word_break_bytes = 60;

    if (content.size() <= max_summary_bytes) return content;

    const size_t safe_limit = utf8_safe_prefix_length(content, max_summary_bytes);
    if (safe_limit == 0) {
        return "...";
    }

    size_t cut = safe_limit;
    while (cut > min_word_break_bytes && content[cut - 1] != ' ') {
        --cut;
    }
    if (cut <= min_word_break_bytes) {
        cut = safe_limit;
    }

    while (cut > 0 && content[cut - 1] == ' ') {
        --cut;
    }

    return content.substr(0, cut) + "...";
}

} // namespace acecode
