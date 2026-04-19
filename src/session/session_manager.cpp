#include "session_manager.hpp"
#include "session_serializer.hpp"

#include <filesystem>
#include <algorithm>

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

void SessionManager::start_session(const std::string& cwd, const std::string& provider, const std::string& model) {
    std::lock_guard<std::mutex> lk(mu_);
    cwd_ = cwd;
    provider_name_ = provider;
    model_name_ = model;
    project_dir_ = SessionStorage::get_project_dir(cwd);
    session_id_.clear();
    jsonl_path_.clear();
    meta_path_str_.clear();
    started_ = true;
    created_ = false;
    finalized_ = false;
    message_count_ = 0;
    last_user_summary_.clear();
    created_at_.clear();
    pending_title_.clear();
}

void SessionManager::ensure_created() {
    // Must be called under lock
    if (created_) return;
    if (!started_) return;

    // Create project directory if needed
    if (!fs::exists(project_dir_)) {
        fs::create_directories(project_dir_);
    }

    session_id_ = SessionStorage::generate_session_id();
    jsonl_path_ = SessionStorage::session_path(project_dir_, session_id_);
    meta_path_str_ = SessionStorage::meta_path(project_dir_, session_id_);
    created_at_ = SessionStorage::now_iso8601();
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
    meta.title = pending_title_;
    SessionStorage::write_meta(meta_path_str_, meta);
}

void SessionManager::on_message(const ChatMessage& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return;

    ensure_created();

    // Append message to JSONL
    SessionStorage::append_message(jsonl_path_, msg);
    message_count_++;

    // Track last user message for summary
    if (msg.role == "user" && !msg.content.empty()) {
        last_user_summary_ = extract_summary(msg.content);
    }

    // Update meta every 5 messages
    if (message_count_ % 5 == 0) {
        update_meta();
    }
}

void SessionManager::finalize() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || finalized_) return;
    finalized_ = true;
    update_meta();
}

std::vector<ChatMessage> SessionManager::resume_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mu_);

    // Read the meta to find which project dir it's in
    // We look in the current project_dir (already set via start_session)
    std::string resume_jsonl = SessionStorage::session_path(project_dir_, session_id);
    std::string resume_meta = SessionStorage::meta_path(project_dir_, session_id);

    if (!fs::exists(resume_jsonl)) {
        return {};
    }

    auto messages = SessionStorage::load_messages(resume_jsonl);

    // Re-activate this session
    session_id_ = session_id;
    jsonl_path_ = resume_jsonl;
    meta_path_str_ = resume_meta;
    created_ = true;
    finalized_ = false;

    // Restore state from meta
    if (fs::exists(resume_meta)) {
        auto meta = SessionStorage::read_meta(resume_meta);
        created_at_ = meta.created_at;
        last_user_summary_ = meta.summary;
        pending_title_ = meta.title;
    }

    message_count_ = static_cast<int>(messages.size());

    return messages;
}

void SessionManager::end_current_session() {
    std::lock_guard<std::mutex> lk(mu_);
    if (created_ && !finalized_) {
        update_meta();
    }
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
    // Keep started_=true, cwd_, provider_name_, model_name_, project_dir_
}

void SessionManager::cleanup_old_sessions(int max_sessions) {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty()) return;

    auto sessions = SessionStorage::list_sessions(project_dir_);
    if (static_cast<int>(sessions.size()) <= max_sessions) return;

    // Sessions are sorted newest-first; remove from the tail
    for (size_t i = static_cast<size_t>(max_sessions); i < sessions.size(); ++i) {
        std::string jsonl = SessionStorage::session_path(project_dir_, sessions[i].id);
        std::string meta = SessionStorage::meta_path(project_dir_, sessions[i].id);
        std::error_code ec;
        fs::remove(jsonl, ec);
        fs::remove(meta, ec);
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
    meta.title = pending_title_;
    SessionStorage::write_meta(meta_path_str_, meta);
}

void SessionManager::set_session_title(std::string title) {
    std::lock_guard<std::mutex> lk(mu_);
    pending_title_ = std::move(title);
    if (created_) {
        update_meta();
    }
}

std::string SessionManager::current_title() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_title_;
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
