#include "session_manager.hpp"
#include "session_serializer.hpp"
#include "session_rewind.hpp"
#include "tool_result_storage.hpp"
#include "turn_timing.hpp"
#include "session_usage_ledger.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

size_t utf8_safe_prefix_length(const std::string& text, size_t max_bytes) {
    const size_t limit = (std::min)(max_bytes, text.size());
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

std::string normalize_permission_mode_name(std::string mode) {
    if (mode == "acceptEdits") mode = "accept-edits";
    if (mode == "accept-edits" || mode == "yolo" || mode == "plan") return mode;
    return "default";
}

std::string normalize_pre_plan_permission_mode_name(std::string mode) {
    if (mode.empty()) return {};
    mode = normalize_permission_mode_name(std::move(mode));
    return mode == "plan" ? std::string{"default"} : mode;
}

std::string plan_file_path_for_session(const std::string& project_dir,
                                       const std::string& session_id) {
    if (project_dir.empty() || session_id.empty()) return {};
    return acecode::path_to_utf8(
        acecode::path_from_utf8(project_dir) / "plans" / (session_id + ".md"));
}

bool is_hidden_goal_context_message(const acecode::ChatMessage& msg) {
    return msg.metadata.is_object() &&
           msg.metadata.value("hidden_goal_context", false);
}

bool is_visible_user_turn_message(const acecode::ChatMessage& msg) {
    return msg.role == "user" &&
           !msg.is_meta &&
           !is_hidden_goal_context_message(msg);
}

bool is_generated_error_title(const std::string& title) {
    std::size_t i = 0;
    while (i < title.size() &&
           std::isspace(static_cast<unsigned char>(title[i])) != 0) {
        ++i;
    }
    if (title.compare(i, 7, "[Error]") == 0) return true;
    if (title.size() - i < 5) return false;
    return std::tolower(static_cast<unsigned char>(title[i])) == 'e' &&
           std::tolower(static_cast<unsigned char>(title[i + 1])) == 'r' &&
           std::tolower(static_cast<unsigned char>(title[i + 2])) == 'r' &&
           std::tolower(static_cast<unsigned char>(title[i + 3])) == 'o' &&
           std::tolower(static_cast<unsigned char>(title[i + 4])) == 'r';
}

std::string turn_timing_dedupe_key(const acecode::ChatMessage& msg) {
    if (!msg.metadata.is_object() || !msg.metadata.contains("turn_timing")) return {};
    return msg.metadata["turn_timing"].dump();
}

void add_retained_turn_timing_message(
    std::unordered_map<std::string, std::vector<acecode::ChatMessage>>& out,
    std::unordered_set<std::string>& seen,
    const std::set<std::string>& retained_user_uuids,
    const acecode::ChatMessage& msg) {
    auto timing = acecode::decode_turn_timing(
        msg.metadata.is_object() ? msg.metadata.value("turn_timing", nlohmann::json{}) : nlohmann::json{});
    if (!timing.has_value()) return;
    if (!retained_user_uuids.count(timing->user_message_uuid)) return;
    const std::string key = turn_timing_dedupe_key(msg);
    if (!key.empty() && !seen.insert(key).second) return;
    out[timing->user_message_uuid].push_back(msg);
}

std::unordered_map<std::string, std::vector<acecode::ChatMessage>>
collect_retained_turn_timing_messages(
    const std::vector<acecode::ChatMessage>& existing_messages,
    const std::vector<acecode::ChatMessage>& retained_messages,
    const std::set<std::string>& retained_user_uuids) {
    std::unordered_map<std::string, std::vector<acecode::ChatMessage>> out;
    std::unordered_set<std::string> seen;
    for (const auto& msg : existing_messages) {
        add_retained_turn_timing_message(out, seen, retained_user_uuids, msg);
    }
    for (const auto& msg : retained_messages) {
        add_retained_turn_timing_message(out, seen, retained_user_uuids, msg);
    }
    return out;
}

void add_usage_to_session_total(acecode::TokenUsage& total,
                                const acecode::TokenUsage& delta) {
    total.prompt_tokens += delta.prompt_tokens;
    total.completion_tokens += delta.completion_tokens;
    total.total_tokens += delta.total_tokens;
    total.cache_read_tokens += delta.cache_read_tokens;
    total.cache_write_tokens += delta.cache_write_tokens;
    total.reasoning_tokens += delta.reasoning_tokens;
    total.has_data = total.has_data || delta.has_data;
}

std::string normalize_path_for_prefix(const std::string& input) {
    std::string out = input;
    for (char& ch : out) {
        if (ch == '\\') ch = '/';
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::string canonical_or_absolute_utf8(const std::string& input) {
    std::error_code ec;
    fs::path p = acecode::path_from_utf8(input);
    fs::path resolved = fs::weakly_canonical(p, ec);
    if (ec) {
        resolved = p.is_absolute() ? p : fs::absolute(p, ec);
        if (ec) resolved = p;
    }
    return normalize_path_for_prefix(acecode::path_to_utf8(resolved));
}

} // namespace

namespace acecode {

void SessionManager::start_session(const std::string& cwd,
                                   const std::string& provider,
                                   const std::string& model,
                                   const std::string& preset_session_id,
                                   const std::string& model_preset,
                                   const std::string& surface,
                                   bool no_workspace) {
    std::lock_guard<std::mutex> lk(mu_);
    release_writer_lease_locked();
    cwd_ = cwd;
    provider_name_ = provider;
    model_name_ = model;
    model_preset_ = model_preset;
    surface_ = surface.empty() ? "unknown" : surface;
    no_workspace_ = no_workspace;
    project_dir_ = SessionStorage::get_project_dir(cwd);
    goal_store_ = std::make_unique<ThreadGoalStore>(project_dir_);
    session_id_ = preset_session_id;
    jsonl_path_.clear();
    meta_path_str_.clear();
    started_ = true;
    created_ = false;
    finalized_ = false;
    message_count_ = 0;
    turn_count_ = 0;
    last_user_summary_.clear();
    created_at_.clear();
    pending_title_.clear();
    title_source_.clear();
    auto_title_generation_attempted_ = false;
    user_title_touched_ = false;
    input_draft_.clear();
    permission_mode_ = "default";
    pre_plan_permission_mode_.clear();
    last_token_usage_ = {};
    session_token_usage_ = {};
    todos_.clear();
    last_error_.clear();
    writer_lease_active_ = false;
    archived_ = false;
    parent_session_id_.clear();
    worktree_ = {};
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
    meta.title_source = title_source_;
    meta.input_draft = input_draft_;
    meta.permission_mode = permission_mode_;
    meta.pre_plan_permission_mode = pre_plan_permission_mode_;
    meta.turn_count = turn_count_;
    meta.last_token_usage = last_token_usage_;
    meta.session_token_usage = session_token_usage_;
    meta.todos = todos_;
    meta.archived = archived_;
    meta.no_workspace = no_workspace_;
    meta.parent_session_id = parent_session_id_;
    meta.worktree = worktree_;
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
    if (is_visible_user_turn_message(msg)) {
        turn_count_++;
    }

    // Track last user message for summary
    if (is_visible_user_turn_message(msg) && !msg.content.empty()) {
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

    const auto existing_messages = SessionStorage::load_messages(jsonl_path_);
    std::unordered_map<std::string, std::vector<ChatMessage>> checkpoints_by_user;
    for (const auto& existing : existing_messages) {
        auto snapshot = FileCheckpointStore::decode_snapshot_message(existing);
        if (!snapshot.has_value()) continue;
        if (retained_user_uuids.count(snapshot->message_uuid)) {
            checkpoints_by_user[snapshot->message_uuid].push_back(existing);
        }
    }
    auto timing_by_user = collect_retained_turn_timing_messages(
        existing_messages, messages, retained_user_uuids);

    std::vector<ChatMessage> rewritten;
    rewritten.reserve(messages.size() + checkpoints_by_user.size() + timing_by_user.size());
    last_user_summary_.clear();
    turn_count_ = 0;
    for (const auto& msg : messages) {
        if (is_file_checkpoint_message(msg)) continue;
        if (is_turn_timing_message(msg)) continue;
        rewritten.push_back(msg);
        if (is_visible_user_turn_message(msg)) {
            turn_count_++;
            if (!msg.content.empty()) {
                last_user_summary_ = extract_summary(msg.content);
            }
            auto it = checkpoints_by_user.find(msg.uuid);
            if (it != checkpoints_by_user.end()) {
                rewritten.insert(rewritten.end(), it->second.begin(), it->second.end());
            }
            auto timing_it = timing_by_user.find(msg.uuid);
            if (timing_it != timing_by_user.end()) {
                rewritten.insert(rewritten.end(), timing_it->second.begin(), timing_it->second.end());
            }
        }
    }

    SessionStorage::write_messages(jsonl_path_, rewritten);
    checkpoint_store_.load_from_messages(project_dir_, session_id_, rewritten);
    message_count_ = static_cast<int>(rewritten.size());
    update_meta();
    return true;
}

bool SessionManager::append_compact_checkpoint(const CompactCheckpoint& checkpoint) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return false;
    if (!ensure_created()) return false;
    if (!created_) return false;

    SessionStorage::append_message(jsonl_path_, encode_compact_checkpoint(checkpoint));
    message_count_++;
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
    const auto replacement_state = reconstruct_tool_result_replacement_state(messages);
    const int replacement_count = apply_tool_result_replacements(messages, replacement_state);
    if (replacement_count > 0) {
        LOG_INFO("[session] reapplied " + std::to_string(replacement_count) +
                 " persisted tool result replacement(s) on resume");
    }

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
        title_source_ = meta.title_source;
        user_title_touched_ = title_source_ == "user" || title_source_ == "legacy";
        input_draft_ = meta.input_draft;
        permission_mode_ = normalize_permission_mode_name(meta.permission_mode);
        pre_plan_permission_mode_ =
            normalize_pre_plan_permission_mode_name(meta.pre_plan_permission_mode);
        turn_count_ = meta.turn_count;
        last_token_usage_ = meta.last_token_usage;
        session_token_usage_ = meta.session_token_usage;
        todos_ = meta.todos;
        archived_ = meta.archived;
        no_workspace_ = meta.no_workspace;
        parent_session_id_ = meta.parent_session_id;
        worktree_ = meta.worktree;
        if (model_preset_.empty()) {
            model_preset_ = meta.model_preset;
        }
    }
    if (created_at_.empty()) {
        created_at_ = SessionStorage::now_iso8601();
    }

    message_count_ = static_cast<int>(messages.size());
    if (turn_count_ <= 0) {
        for (const auto& msg : messages) {
            if (is_visible_user_turn_message(msg)) turn_count_++;
        }
    }
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
    turn_count_ = 0;
    last_user_summary_.clear();
    created_at_.clear();
    pending_title_.clear();
    title_source_.clear();
    auto_title_generation_attempted_ = false;
    user_title_touched_ = false;
    input_draft_.clear();
    last_token_usage_ = {};
    session_token_usage_ = {};
    todos_.clear();
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

    const std::string previous_session_id = session_id_;
    const std::string new_session_id = SessionStorage::generate_session_id();
    auto checkpoint_meta = checkpoint_store_.fork_to_session(new_session_id, retained_user_uuids);
    auto timing_by_user = collect_retained_turn_timing_messages(
        SessionStorage::load_messages(jsonl_path_), retained_prefix, retained_user_uuids);

    release_writer_lease_locked();

    session_id_ = new_session_id;
    jsonl_path_ = SessionStorage::session_path(project_dir_, session_id_);
    meta_path_str_ = SessionStorage::meta_path(project_dir_, session_id_);
    created_at_ = SessionStorage::now_iso8601();
    created_ = false;
    finalized_ = false;
    message_count_ = 0;
    turn_count_ = 0;
    last_token_usage_ = {};
    session_token_usage_ = {};
    last_user_summary_.clear();
    input_draft_.clear();
    todos_.clear();

    if (!acquire_writer_lease_locked()) {
        return {};
    }
    created_ = true;

    for (auto it = retained_prefix.rbegin(); it != retained_prefix.rend(); ++it) {
        if (is_visible_user_turn_message(*it) && !it->content.empty()) {
            last_user_summary_ = extract_summary(it->content);
            break;
        }
    }

    fs::create_directories(project_dir_);
    update_meta();

    for (const auto& msg : retained_prefix) {
        if (is_file_checkpoint_message(msg)) continue;
        if (is_turn_timing_message(msg)) continue;
        SessionStorage::append_message(jsonl_path_, msg);
        message_count_++;
        if (is_visible_user_turn_message(msg)) {
            turn_count_++;
            auto timing_it = timing_by_user.find(msg.uuid);
            if (timing_it != timing_by_user.end()) {
                for (const auto& timing_msg : timing_it->second) {
                    SessionStorage::append_message(jsonl_path_, timing_msg);
                    message_count_++;
                }
            }
        }
    }
    for (const auto& msg : checkpoint_meta) {
        SessionStorage::append_message(jsonl_path_, msg);
        message_count_++;
    }
    update_meta();
    if (goal_store_ && !previous_session_id.empty()) {
        std::string goal_error;
        if (!goal_store_->copy_goal_reset_usage(previous_session_id, session_id_, &goal_error)) {
            LOG_WARN("[session] failed to copy goal for fork: " + goal_error);
        }
    }
    return session_id_;
}

std::vector<ChatMessage> SessionManager::load_active_messages() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_ || !created_ || jsonl_path_.empty()) {
        return {};
    }
    return SessionStorage::load_messages(jsonl_path_);
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
    std::set<std::string> retained_user_uuids;
    for (const auto& msg : retained_prefix) {
        if (msg.role == "user" && !msg.uuid.empty()) {
            retained_user_uuids.insert(msg.uuid);
        }
    }
    auto timing_by_user = collect_retained_turn_timing_messages(
        jsonl_path_.empty() ? std::vector<ChatMessage>{} : SessionStorage::load_messages(jsonl_path_),
        retained_prefix,
        retained_user_uuids);

    // 写新 jsonl(过滤 file_checkpoint 元消息;新 session 不继承 checkpoints)
    int count = 0;
    int turn_count = 0;
    std::string last_user_summary;
    bool io_error = false;
    try {
        for (const auto& msg : retained_prefix) {
            if (is_file_checkpoint_message(msg)) continue;
            if (is_turn_timing_message(msg)) continue;
            SessionStorage::append_message(new_jsonl, msg);
            count++;
            if (is_visible_user_turn_message(msg)) {
                turn_count++;
                auto timing_it = timing_by_user.find(msg.uuid);
                if (timing_it != timing_by_user.end()) {
                    for (const auto& timing_msg : timing_it->second) {
                        SessionStorage::append_message(new_jsonl, timing_msg);
                        count++;
                    }
                }
            }
            if (is_visible_user_turn_message(msg) && !msg.content.empty()) {
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
    meta.turn_count      = turn_count;
    meta.summary         = last_user_summary;
    meta.provider        = provider_name_;
    meta.model           = model_name_;
    meta.model_preset    = model_preset_;
    meta.title           = title;
    meta.title_source    = title.empty() ? std::string{} : "user";
    meta.input_draft     = std::string{};
    meta.permission_mode = permission_mode_;
    meta.pre_plan_permission_mode = pre_plan_permission_mode_;
    meta.forked_from     = forked_from_id;
    meta.fork_message_id = fork_message_id;
    meta.no_workspace    = no_workspace_;
    SessionStorage::write_meta(new_meta, meta);
    if (goal_store_ && !forked_from_id.empty()) {
        std::string goal_error;
        if (!goal_store_->copy_goal_reset_usage(forked_from_id, new_session_id, &goal_error)) {
            LOG_WARN("[session] failed to copy goal for session fork: " + goal_error);
        }
    }

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
        fs::remove_all(path_from_utf8(project_dir_) / id, ec);
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

std::string SessionManager::ensure_active_session_id() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return {};
    if (!ensure_created()) return {};
    return session_id_;
}

std::string SessionManager::ensure_tool_results_dir() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return {};
    if (!ensure_created()) return {};
    const std::string dir = tool_results_dir_for_session(project_dir_, session_id_);
    std::error_code ec;
    fs::create_directories(path_from_utf8(dir), ec);
    if (ec) {
        LOG_WARN("[session] failed to create tool results dir " + dir + ": " + ec.message());
        return {};
    }
    return dir;
}

bool SessionManager::is_tool_result_artifact_path(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty() || session_id_.empty() || path.empty()) return false;
    const std::string dir = canonical_or_absolute_utf8(
        tool_results_dir_for_session(project_dir_, session_id_));
    const std::string resolved = canonical_or_absolute_utf8(path);
    if (dir.empty() || resolved.empty()) return false;
    if (resolved.size() <= dir.size()) return false;
    if (resolved.compare(0, dir.size(), dir) != 0) return false;
    const char next = resolved[dir.size()];
    return next == '/' || next == '\\';
}

ThreadGoalStore* SessionManager::goal_store() {
    std::lock_guard<std::mutex> lk(mu_);
    if (goal_store_ && !goal_store_->available()) {
        std::string goal_error;
        if (!goal_store_->initialize(&goal_error)) {
            LOG_WARN("[session] goal store initialization failed: " + goal_error);
            goal_store_.reset();
        }
    }
    return goal_store_.get();
}

ThreadGoalStore* SessionManager::existing_goal_store() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!goal_store_) return nullptr;
    if (!goal_store_->available()) {
        const auto db_path = ThreadGoalStore::database_path_for_project(project_dir_);
        if (!fs::exists(db_path)) return nullptr;
        std::string goal_error;
        if (!goal_store_->initialize(&goal_error)) {
            LOG_WARN("[session] goal store initialization failed: " + goal_error);
            goal_store_.reset();
        }
    }
    return goal_store_.get();
}

const ThreadGoalStore* SessionManager::goal_store() const {
    std::lock_guard<std::mutex> lk(mu_);
    return goal_store_.get();
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
    meta.turn_count = turn_count_;
    meta.summary = last_user_summary_;
    meta.provider = provider_name_;
    meta.model = model_name_;
    meta.model_preset = model_preset_;
    meta.title = pending_title_;
    meta.title_source = title_source_;
    meta.input_draft = input_draft_;
    meta.permission_mode = permission_mode_;
    meta.pre_plan_permission_mode = pre_plan_permission_mode_;
    meta.last_token_usage = last_token_usage_;
    meta.session_token_usage = session_token_usage_;
    meta.todos = todos_;
    meta.archived = archived_;
    meta.no_workspace = no_workspace_;
    meta.parent_session_id = parent_session_id_;
    meta.worktree = worktree_;
    SessionStorage::write_meta(meta_path_str_, meta);
    refresh_writer_lease_locked();
}

void SessionManager::set_session_title(std::string title) {
    std::lock_guard<std::mutex> lk(mu_);
    pending_title_ = std::move(title);
    title_source_ = pending_title_.empty() ? std::string{} : "user";
    user_title_touched_ = true;
    if (created_) {
        update_meta();
    }
}

bool SessionManager::try_set_generated_session_title(std::string title) {
    std::lock_guard<std::mutex> lk(mu_);
    if (title.empty()) return false;
    if (user_title_touched_) return false;
    if (!pending_title_.empty() && title_source_ != "generated") return false;
    const bool incoming_error = is_generated_error_title(title);
    const bool current_generated_error =
        title_source_ == "generated" && is_generated_error_title(pending_title_);
    if (incoming_error && title_source_ == "generated" &&
        !pending_title_.empty() && !current_generated_error) {
        return false;
    }
    pending_title_ = std::move(title);
    title_source_ = "generated";
    if (created_) {
        update_meta();
    }
    return true;
}

bool SessionManager::mark_auto_title_generation_started() {
    std::lock_guard<std::mutex> lk(mu_);
    const bool retry_generated_error =
        title_source_ == "generated" && is_generated_error_title(pending_title_);
    if (auto_title_generation_attempted_ && !retry_generated_error) return false;
    if (turn_count_ > 0 && !retry_generated_error) return false;
    if (user_title_touched_) return false;
    if (!pending_title_.empty() && title_source_ != "generated") return false;
    auto_title_generation_attempted_ = true;
    return true;
}

void SessionManager::set_session_archived(bool archived) {
    std::lock_guard<std::mutex> lk(mu_);
    archived_ = archived;
    if (created_) {
        update_meta();
    }
}

void SessionManager::set_parent_session_id(std::string parent_id) {
    std::lock_guard<std::mutex> lk(mu_);
    parent_session_id_ = std::move(parent_id);
    if (created_) {
        update_meta();
    }
}

std::string SessionManager::current_parent_session_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return parent_session_id_;
}

void SessionManager::set_active_worktree(const WorktreeSessionInfo& info) {
    std::lock_guard<std::mutex> lk(mu_);
    worktree_ = info;
    if (created_) {
        update_meta();
    }
}

void SessionManager::clear_active_worktree() {
    std::lock_guard<std::mutex> lk(mu_);
    worktree_ = {};
    if (created_) {
        update_meta();
    }
}

WorktreeSessionInfo SessionManager::active_worktree() const {
    std::lock_guard<std::mutex> lk(mu_);
    return worktree_;
}

std::string SessionManager::current_title() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_title_;
}

std::string SessionManager::current_title_source() const {
    std::lock_guard<std::mutex> lk(mu_);
    return title_source_;
}

void SessionManager::set_input_draft(std::string draft) {
    std::lock_guard<std::mutex> lk(mu_);
    input_draft_ = std::move(draft);
    if (!created_ && started_ && !input_draft_.empty()) {
        ensure_created();
    }
    if (!created_) return;

    SessionMeta meta = SessionStorage::read_meta(meta_path_str_);
    if (meta.id.empty()) {
        meta.id = session_id_;
        meta.cwd = cwd_;
        meta.created_at = created_at_.empty() ? SessionStorage::now_iso8601() : created_at_;
        meta.updated_at = meta.created_at;
        meta.message_count = message_count_;
        meta.turn_count = turn_count_;
        meta.summary = last_user_summary_;
        meta.provider = provider_name_;
        meta.model = model_name_;
        meta.model_preset = model_preset_;
        meta.title = pending_title_;
        meta.title_source = title_source_;
        meta.permission_mode = permission_mode_;
        meta.pre_plan_permission_mode = pre_plan_permission_mode_;
        meta.last_token_usage = last_token_usage_;
        meta.session_token_usage = session_token_usage_;
        meta.todos = todos_;
        meta.archived = archived_;
        meta.no_workspace = no_workspace_;
    }
    meta.input_draft = input_draft_;
    SessionStorage::write_meta(meta_path_str_, meta);
}

std::string SessionManager::current_input_draft() const {
    std::lock_guard<std::mutex> lk(mu_);
    return input_draft_;
}

void SessionManager::set_permission_mode(std::string mode, bool persist_immediately) {
    std::lock_guard<std::mutex> lk(mu_);
    permission_mode_ = normalize_permission_mode_name(std::move(mode));
    if (permission_mode_ != "plan") {
        pre_plan_permission_mode_.clear();
    }
    if (persist_immediately) {
        if (!created_ && started_) {
            ensure_created();
        }
        if (created_) {
            update_meta();
        }
    }
}

std::string SessionManager::current_permission_mode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return permission_mode_;
}

void SessionManager::set_pre_plan_permission_mode(std::string mode, bool persist_immediately) {
    std::lock_guard<std::mutex> lk(mu_);
    pre_plan_permission_mode_ = normalize_pre_plan_permission_mode_name(std::move(mode));
    if (persist_immediately) {
        if (!created_ && started_) {
            ensure_created();
        }
        if (created_) {
            update_meta();
        }
    }
}

std::string SessionManager::current_pre_plan_permission_mode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pre_plan_permission_mode_;
}

void SessionManager::set_todos(std::vector<TodoItem> todos, bool persist_immediately) {
    std::lock_guard<std::mutex> lk(mu_);
    todos_ = std::move(todos);
    if (persist_immediately) {
        if (!created_ && started_ && !todos_.empty()) {
            ensure_created();
        }
        if (created_) {
            update_meta();
        }
    }
}

std::vector<TodoItem> SessionManager::current_todos() const {
    std::lock_guard<std::mutex> lk(mu_);
    return todos_;
}

std::string SessionManager::ensure_plan_file_path() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return {};
    if (!ensure_created()) return {};
    const std::string path = plan_file_path_for_session(project_dir_, session_id_);
    if (path.empty()) return {};
    std::error_code ec;
    fs::create_directories(path_from_utf8(path).parent_path(), ec);
    if (ec) {
        LOG_WARN("[session] failed to create plan directory for " + path + ": " + ec.message());
        return {};
    }
    if (!fs::exists(path_from_utf8(path), ec)) {
        if (!atomic_write_file(path, "")) {
            LOG_WARN("[session] failed to create plan file " + path);
            return {};
        }
    }
    return path;
}

std::string SessionManager::current_plan_file_path() const {
    std::lock_guard<std::mutex> lk(mu_);
    return plan_file_path_for_session(project_dir_, session_id_);
}

std::string SessionManager::read_plan_file() const {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string path = plan_file_path_for_session(project_dir_, session_id_);
    if (path.empty()) return {};
    std::ifstream ifs(path_from_utf8(path), std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool SessionManager::write_plan_file(const std::string& content, std::string* error) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!started_) {
            if (error) *error = "session is not started";
            return false;
        }
        if (!ensure_created()) {
            if (error) *error = "session is not available";
            return false;
        }
        path = plan_file_path_for_session(project_dir_, session_id_);
    }
    if (path.empty()) {
        if (error) *error = "plan file path is not available";
        return false;
    }
    std::error_code ec;
    fs::create_directories(path_from_utf8(path).parent_path(), ec);
    if (ec) {
        if (error) *error = "failed to create plan directory: " + ec.message();
        return false;
    }
    try {
        if (atomic_write_file(path, content)) {
            return true;
        }
        if (error) *error = "failed to write plan file atomically";
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool SessionManager::is_plan_file_path(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (project_dir_.empty() || session_id_.empty() || path.empty()) return false;
    const std::string plan_path = canonical_or_absolute_utf8(
        plan_file_path_for_session(project_dir_, session_id_));
    const std::string resolved = canonical_or_absolute_utf8(path);
    return !plan_path.empty() && !resolved.empty() && plan_path == resolved;
}

void SessionManager::record_token_usage(const TokenUsage& usage) {
    std::lock_guard<std::mutex> lk(mu_);
    last_token_usage_ = usage;
    add_usage_to_session_total(session_token_usage_, usage);
    if (!created_ && started_) {
        ensure_created();
    }
    if (created_) {
        const auto timestamp_ms = usage_now_unix_ms();
        UsageLedgerRecord record;
        record.timestamp_ms = timestamp_ms;
        record.timestamp = usage_iso8601_from_unix_ms(timestamp_ms);
        record.session_id = session_id_;
        record.cwd = cwd_;
        record.provider = provider_name_;
        record.model = model_name_;
        record.model_preset = model_preset_;
        record.surface = surface_;
        record.usage = usage;
        append_usage_ledger_record(project_dir_, record);
        update_meta();
    }
}

TokenUsage SessionManager::current_last_token_usage() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_token_usage_;
}

TokenUsage SessionManager::current_session_token_usage() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_token_usage_;
}

int SessionManager::current_turn_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return turn_count_;
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
