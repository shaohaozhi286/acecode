#include "session_control_service.hpp"

#include "session_pin_store.hpp"
#include "session_registry.hpp"
#include "session_storage.hpp"
#include "../utils/encoding.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace acecode {

namespace {

using nlohmann::json;

constexpr std::size_t kMaxPageSize = 20;
constexpr std::size_t kMaxReadBytes = 8192;
constexpr std::size_t kListSummaryBytes = 240;
constexpr std::size_t kWaitEventBytes = 1200;
constexpr std::size_t kMaxWaitEvents = 8;

std::string trim_and_limit(std::string value, std::size_t max_bytes,
                           bool* truncated = nullptr) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const std::string limited = truncate_utf8_prefix(value, max_bytes);
    if (truncated) *truncated = limited.size() < value.size();
    return limited;
}

bool valid_title(const std::string& title) {
    if (title.size() > 160) return false;
    return std::none_of(title.begin(), title.end(), [](unsigned char ch) {
        return ch < 0x20 && ch != '\t';
    });
}

std::string cursor_hex_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (const unsigned char ch : value) {
        out.push_back(digits[ch >> 4]);
        out.push_back(digits[ch & 0x0f]);
    }
    return out;
}

std::optional<std::string> cursor_hex_decode(const std::string& value) {
    if (value.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int hi = nibble(value[i]);
        const int lo = nibble(value[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string page_key(const std::string& updated_at, const std::string& id) {
    return updated_at + '\n' + id;
}

struct ScopedSession {
    SessionMeta meta;
    std::shared_ptr<SessionEntry> active;
    std::string project_dir;
};

std::optional<ScopedSession> resolve_scoped_session(
    const SessionControlService::Deps& deps,
    const SessionControlScope& scope,
    const std::string& session_id,
    bool allow_subagent = false) {
    if (scope.cwd.empty() || session_id.empty()) return std::nullopt;
    const std::string project_dir = SessionStorage::get_project_dir(scope.cwd);
    SessionMeta meta = SessionStorage::read_meta(
        SessionStorage::meta_path(project_dir, session_id));
    std::shared_ptr<SessionEntry> active;
    if (deps.registry) active = deps.registry->acquire(session_id);

    const std::string expected_hash =
        SessionStorage::compute_project_hash(scope.cwd);
    if (active) {
        const bool matches = !active->no_workspace &&
            ((active->workspace_hash.empty() && active->cwd == scope.cwd) ||
             active->workspace_hash == expected_hash);
        if (!matches) active.reset();
    }
    if (meta.id.empty() && !active) return std::nullopt;
    if (!meta.id.empty() && meta.no_workspace) return std::nullopt;
    const std::string parent = !meta.id.empty()
        ? meta.parent_session_id
        : (active ? active->parent_session_id : std::string{});
    if (!allow_subagent && !parent.empty()) return std::nullopt;

    if (meta.id.empty() && active) {
        const std::string now = SessionStorage::now_iso8601();
        meta.id = active->id;
        meta.cwd = active->cwd;
        meta.created_at = now;
        meta.updated_at = now;
        meta.provider = active->provider;
        meta.model = active->model;
        meta.model_preset = active->model_state.name;
        meta.parent_session_id = active->parent_session_id;
        if (active->sm) {
            meta.title = active->sm->current_title();
            meta.title_source = active->sm->current_title_source();
            meta.turn_count = active->sm->current_turn_count();
            meta.permission_mode = active->sm->current_permission_mode();
        }
    }
    return ScopedSession{std::move(meta), std::move(active), project_dir};
}

json compact_session_json(const SessionMeta& meta,
                          const SessionInfo* active_info,
                          bool pinned) {
    bool summary_truncated = false;
    const std::string summary = trim_and_limit(
        active_info && !active_info->summary.empty()
            ? active_info->summary
            : meta.summary,
        kListSummaryBytes, &summary_truncated);
    const bool active = active_info != nullptr;
    const bool busy = active && active_info->busy;
    return json{
        {"id", meta.id.empty() && active_info ? active_info->id : meta.id},
        {"title", active_info && !active_info->title.empty()
                      ? active_info->title : meta.title},
        {"summary", summary},
        {"summary_truncated", summary_truncated},
        {"updated_at", active_info && !active_info->updated_at.empty()
                           ? active_info->updated_at : meta.updated_at},
        {"active", active},
        {"busy", busy},
        {"archived", meta.archived},
        {"pinned", pinned},
    };
}

std::string event_kind_name(SessionEventKind kind) {
    switch (kind) {
        case SessionEventKind::Message: return "message";
        case SessionEventKind::SessionUpdated: return "session_updated";
        case SessionEventKind::BusyChanged: return "busy_changed";
        case SessionEventKind::Done: return "done";
        case SessionEventKind::Error: return "error";
        default: return {};
    }
}

json compact_wait_event(const SessionEvent& event) {
    json out{{"seq", event.seq},
             {"type", event_kind_name(event.kind)}};
    if (event.kind == SessionEventKind::Message) {
        out["role"] = event.payload.value("role", std::string{});
        bool truncated = false;
        out["content"] = trim_and_limit(
            event.payload.value("content", std::string{}),
            kWaitEventBytes, &truncated);
        out["truncated"] = truncated;
    } else if (event.payload.is_object()) {
        for (const char* key : {"busy", "outcome", "title", "reason"}) {
            if (event.payload.contains(key)) out[key] = event.payload[key];
        }
    }
    return out;
}

bool is_relevant_wait_event(SessionEventKind kind) {
    return kind == SessionEventKind::Message ||
           kind == SessionEventKind::SessionUpdated ||
           kind == SessionEventKind::BusyChanged ||
           kind == SessionEventKind::Done ||
           kind == SessionEventKind::Error;
}

bool message_matches_id(const ChatMessage& message, const std::string& id) {
    if (id.empty()) return false;
    if (message.uuid == id) return true;
    if (message.metadata.is_object()) {
        for (const char* key : {"id", "message_id"}) {
            if (message.metadata.value(key, std::string{}) == id) return true;
        }
    }
    return false;
}

} // namespace

SessionControlResult SessionControlResult::ok(json value) {
    SessionControlResult result;
    result.success = true;
    result.value = std::move(value);
    return result;
}

SessionControlResult SessionControlResult::fail(std::string error) {
    SessionControlResult result;
    result.error = std::move(error);
    return result;
}

SessionControlService::SessionControlService(Deps deps)
    : deps_(deps) {}

SessionControlResult SessionControlService::list(
    const SessionControlScope& scope,
    std::size_t page_size,
    const std::string& cursor,
    bool include_archived) const {
    if (scope.cwd.empty()) return SessionControlResult::fail("workspace scope is unavailable");
    page_size = (std::max)(std::size_t{1},
                          (std::min)(page_size, kMaxPageSize));
    const std::string project_dir = SessionStorage::get_project_dir(scope.cwd);

    std::unordered_map<std::string, SessionMeta> metas;
    for (auto meta : SessionStorage::list_sessions(project_dir)) {
        if (!meta.parent_session_id.empty() || meta.no_workspace ||
            meta.archived != include_archived) {
            continue;
        }
        metas.emplace(meta.id, std::move(meta));
    }

    std::unordered_map<std::string, SessionInfo> active;
    if (deps_.client) {
        const std::string expected_hash =
            SessionStorage::compute_project_hash(scope.cwd);
        for (auto info : deps_.client->list_sessions()) {
            if (info.no_workspace || !info.parent_session_id.empty() ||
                info.workspace_hash != expected_hash) {
                continue;
            }
            auto found = metas.find(info.id);
            if (found != metas.end() && found->second.archived != include_archived) {
                continue;
            }
            if (found == metas.end()) {
                if (include_archived) continue;
                SessionMeta meta;
                meta.id = info.id;
                meta.cwd = info.cwd;
                meta.created_at = info.created_at;
                meta.updated_at = info.updated_at;
                meta.summary = info.summary;
                meta.title = info.title;
                metas.emplace(meta.id, std::move(meta));
            }
            active.emplace(info.id, std::move(info));
        }
    }

    struct Row {
        SessionMeta meta;
        const SessionInfo* active = nullptr;
    };
    std::vector<Row> rows;
    rows.reserve(metas.size());
    for (auto& [id, meta] : metas) {
        auto it = active.find(id);
        rows.push_back({meta, it == active.end() ? nullptr : &it->second});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& lhs, const Row& rhs) {
        const std::string lhs_updated = lhs.active && !lhs.active->updated_at.empty()
            ? lhs.active->updated_at : lhs.meta.updated_at;
        const std::string rhs_updated = rhs.active && !rhs.active->updated_at.empty()
            ? rhs.active->updated_at : rhs.meta.updated_at;
        if (lhs_updated != rhs_updated) return lhs_updated > rhs_updated;
        return lhs.meta.id < rhs.meta.id;
    });

    std::string cursor_key;
    if (!cursor.empty()) {
        auto decoded = cursor_hex_decode(cursor);
        if (!decoded || decoded->find('\n') == std::string::npos) {
            return SessionControlResult::fail("invalid cursor");
        }
        cursor_key = *decoded;
    }
    std::size_t start = 0;
    if (!cursor_key.empty()) {
        const auto split = cursor_key.find('\n');
        const std::string cursor_updated = cursor_key.substr(0, split);
        const std::string cursor_id = cursor_key.substr(split + 1);
        while (start < rows.size()) {
            const auto& row = rows[start];
            const std::string updated = row.active && !row.active->updated_at.empty()
                ? row.active->updated_at : row.meta.updated_at;
            if (updated < cursor_updated ||
                (updated == cursor_updated && row.meta.id > cursor_id)) {
                break;
            }
            ++start;
        }
    }

    const auto pin_path = path_from_utf8(project_dir) / "pinned_sessions.json";
    const auto pin_state = session_pins::read_pinned_sessions_state(pin_path);
    const auto is_pinned = [&pin_state](const std::string& id) {
        return std::find(pin_state.session_ids.begin(),
                         pin_state.session_ids.end(), id) !=
               pin_state.session_ids.end();
    };

    json items = json::array();
    std::size_t index = start;
    for (; index < rows.size() && items.size() < page_size; ++index) {
        items.push_back(compact_session_json(
            rows[index].meta, rows[index].active,
            is_pinned(rows[index].meta.id)));
    }
    std::string next_cursor;
    if (index < rows.size() && index > start) {
        const auto& last = rows[index - 1];
        const std::string updated = last.active && !last.active->updated_at.empty()
            ? last.active->updated_at : last.meta.updated_at;
        next_cursor = cursor_hex_encode(page_key(updated, last.meta.id));
    }
    return SessionControlResult::ok(json{
        {"items", std::move(items)},
        {"next_cursor", next_cursor.empty() ? json(nullptr) : json(next_cursor)},
        {"has_more", index < rows.size()},
    });
}

SessionControlResult SessionControlService::read(
    const SessionControlScope& scope,
    const std::string& session_id,
    std::size_t max_bytes) const {
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped) return SessionControlResult::fail("session is unavailable in this workspace scope");
    max_bytes = (std::max)(std::size_t{256},
                          (std::min)(max_bytes, kMaxReadBytes));

    std::vector<ChatMessage> messages;
    if (scoped->active && scoped->active->sm) {
        messages = scoped->active->sm->load_active_messages();
    }
    if (messages.empty()) {
        const auto candidates = SessionStorage::find_session_files(
            scoped->project_dir, session_id);
        if (!candidates.empty()) {
            messages = SessionStorage::load_messages(candidates.front().jsonl_path);
        }
    }

    std::string last_user;
    std::string last_assistant;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (last_assistant.empty() && it->role == "assistant" &&
            !it->content.empty()) {
            last_assistant = it->content;
        }
        if (last_user.empty() && it->role == "user" && !it->content.empty() &&
            !it->metadata.value("hidden_goal_context", false)) {
            last_user = it->content;
        }
        if (!last_user.empty() && !last_assistant.empty()) break;
    }

    const std::size_t user_budget = max_bytes / 3;
    const std::size_t assistant_budget = max_bytes - user_budget;
    bool user_truncated = false;
    bool assistant_truncated = false;
    last_user = trim_and_limit(last_user, user_budget, &user_truncated);
    last_assistant = trim_and_limit(
        last_assistant, assistant_budget, &assistant_truncated);

    std::uint64_t cursor = 0;
    bool busy = false;
    if (scoped->active && scoped->active->loop) {
        cursor = scoped->active->loop->events().current_seq();
        busy = scoped->active->loop->is_busy();
    }
    return SessionControlResult::ok(json{
        {"id", session_id},
        {"title", scoped->meta.title},
        {"summary", trim_and_limit(scoped->meta.summary, kListSummaryBytes)},
        {"updated_at", scoped->meta.updated_at},
        {"active", scoped->active != nullptr},
        {"busy", busy},
        {"archived", scoped->meta.archived},
        {"message_count", scoped->meta.message_count},
        {"turn_count", scoped->meta.turn_count},
        {"last_user", last_user},
        {"last_assistant", last_assistant},
        {"truncated", user_truncated || assistant_truncated},
        {"cursor", cursor},
    });
}

SessionControlResult SessionControlService::wait(
    const SessionControlScope& scope,
    const std::string& session_id,
    std::uint64_t since_seq,
    int timeout_seconds,
    const std::atomic<bool>* abort_flag) const {
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped) return SessionControlResult::fail("session is unavailable in this workspace scope");
    if (!scoped->active || !scoped->active->loop || !deps_.client) {
        return SessionControlResult::ok(json{
            {"events", json::array()},
            {"cursor", 0},
            {"timed_out", false},
            {"active", false},
            {"busy", false},
        });
    }
    timeout_seconds = (std::max)(0, (std::min)(timeout_seconds, 60));

    std::mutex mu;
    std::condition_variable cv;
    std::vector<json> events;
    bool terminal = false;
    std::uint64_t latest = since_seq;
    const auto subscription = deps_.client->subscribe(
        session_id,
        [&](const SessionEvent& event) {
            if (!is_relevant_wait_event(event.kind)) return;
            std::lock_guard<std::mutex> lock(mu);
            latest = (std::max)(latest, event.seq);
            if (events.size() < kMaxWaitEvents) {
                events.push_back(compact_wait_event(event));
            }
            if (event.kind == SessionEventKind::Done ||
                event.kind == SessionEventKind::Error ||
                (event.kind == SessionEventKind::BusyChanged &&
                 !event.payload.value("busy", true))) {
                terminal = true;
            }
            cv.notify_all();
        },
        since_seq);
    if (subscription == 0) {
        return SessionControlResult::fail("session event stream is unavailable");
    }

    bool timed_out = false;
    bool aborted = false;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(timeout_seconds);
    {
        std::unique_lock<std::mutex> lock(mu);
        const bool currently_busy = scoped->active->loop->is_busy();
        if (!currently_busy) terminal = true;
        while (events.empty() && !terminal) {
            if (abort_flag && abort_flag->load()) {
                aborted = true;
                break;
            }
            if (timeout_seconds == 0) {
                timed_out = true;
                break;
            }
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) {
                timed_out = events.empty() && !terminal;
                break;
            }
        }
    }
    deps_.client->unsubscribe(session_id, subscription);
    latest = (std::max)(latest,
        scoped->active->loop->events().current_seq());

    json event_array = json::array();
    for (auto& event : events) event_array.push_back(std::move(event));
    return SessionControlResult::ok(json{
        {"events", std::move(event_array)},
        {"cursor", latest},
        {"timed_out", timed_out},
        {"aborted", aborted},
        {"active", true},
        {"busy", scoped->active->loop->is_busy()},
    });
}

SessionControlResult SessionControlService::create(
    const SessionControlScope& scope,
    const std::string& title,
    const std::string& model_name) const {
    if (!deps_.registry || scope.cwd.empty()) {
        return SessionControlResult::fail("session creation is unavailable");
    }
    if (!valid_title(title)) return SessionControlResult::fail("invalid title");
    SessionOptions options;
    options.cwd = scope.cwd;
    options.model_name = model_name;
    std::string id;
    try {
        id = deps_.registry->create(options);
    } catch (const std::exception& error) {
        return SessionControlResult::fail(
            std::string("failed to create session: ") + error.what());
    }
    auto entry = deps_.registry->acquire(id);
    if (!entry || !entry->sm || entry->sm->ensure_active_session_id().empty()) {
        if (deps_.client) deps_.client->destroy_session(id);
        return SessionControlResult::fail("failed to persist new session");
    }
    if (!title.empty()) entry->sm->set_session_title(title);
    return SessionControlResult::ok(json{{"session_id", id}, {"title", title}});
}

SessionControlResult SessionControlService::fork(
    const SessionControlScope& scope,
    const std::string& session_id,
    const std::string& title,
    const std::string& at_message_id) const {
    if (!valid_title(title)) return SessionControlResult::fail("invalid title");
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped) return SessionControlResult::fail("session is unavailable in this workspace scope");
    if (!scoped->active) {
        SessionOptions options;
        options.cwd = scope.cwd;
        if (!deps_.client || !deps_.client->resume_session(session_id, options)) {
            return SessionControlResult::fail("failed to resume source session");
        }
        scoped = resolve_scoped_session(deps_, scope, session_id);
    }
    if (!scoped || !scoped->active || !scoped->active->sm) {
        return SessionControlResult::fail("source session is unavailable");
    }

    auto messages = scoped->active->sm->load_active_messages();
    if (!at_message_id.empty()) {
        auto found = std::find_if(messages.begin(), messages.end(),
            [&](const ChatMessage& message) {
                return message_matches_id(message, at_message_id);
            });
        if (found == messages.end()) {
            return SessionControlResult::fail("fork message was not found");
        }
        messages.erase(std::next(found), messages.end());
    }
    const std::string effective_title = title.empty()
        ? (scoped->meta.title.empty() ? std::string{"Fork"}
                                     : scoped->meta.title + " (fork)")
        : title;
    const std::string new_id = scoped->active->sm->fork_session_to_new_id(
        messages, effective_title, session_id, at_message_id);
    if (new_id.empty()) return SessionControlResult::fail("failed to write fork");
    SessionOptions options;
    options.cwd = scope.cwd;
    if (deps_.client && !deps_.client->resume_session(new_id, options)) {
        return SessionControlResult::fail("fork was written but could not be activated");
    }
    return SessionControlResult::ok(json{
        {"session_id", new_id},
        {"forked_from", session_id},
        {"fork_message_id", at_message_id},
        {"title", effective_title},
    });
}

SessionControlResult SessionControlService::send(
    const SessionControlScope& scope,
    const std::string& session_id,
    const std::string& message,
    bool steer_if_busy) const {
    if (message.empty()) return SessionControlResult::fail("message is required");
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped) return SessionControlResult::fail("session is unavailable in this workspace scope");
    if (!scoped->active) {
        SessionOptions options;
        options.cwd = scope.cwd;
        if (!deps_.client || !deps_.client->resume_session(session_id, options)) {
            return SessionControlResult::fail("failed to resume target session");
        }
        scoped = resolve_scoped_session(deps_, scope, session_id);
    }
    if (!scoped || !scoped->active || !deps_.client) {
        return SessionControlResult::fail("target session is unavailable");
    }
    if (steer_if_busy && scoped->active->loop &&
        scoped->active->loop->is_busy()) {
        const std::string turn_id = scoped->active->loop->active_turn_id();
        if (!turn_id.empty()) {
            UserInput input;
            input.text = message;
            auto steered = deps_.client->steer_input(session_id, turn_id, input);
            if (steered.accepted()) {
                return SessionControlResult::ok(json{
                    {"session_id", session_id},
                    {"delivery", "steered"},
                    {"turn_id", steered.turn_id},
                });
            }
        }
    }
    if (!deps_.client->send_input(session_id, message)) {
        return SessionControlResult::fail("failed to queue message");
    }
    return SessionControlResult::ok(json{
        {"session_id", session_id},
        {"delivery", "queued"},
    });
}

SessionControlResult SessionControlService::interrupt(
    const SessionControlScope& scope,
    const std::string& session_id) const {
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped) return SessionControlResult::fail("session is unavailable in this workspace scope");
    if (!scoped->active || !deps_.client) {
        return SessionControlResult::ok(json{
            {"session_id", session_id}, {"interrupted", false},
            {"reason", "session is not active"},
        });
    }
    deps_.client->abort(session_id);
    return SessionControlResult::ok(json{
        {"session_id", session_id}, {"interrupted", true},
    });
}

SessionControlResult SessionControlService::set_title(
    const SessionControlScope& scope,
    const std::string& session_id,
    const std::string& title) const {
    if (!valid_title(title)) return SessionControlResult::fail("invalid title");
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped) return SessionControlResult::fail("session is unavailable in this workspace scope");
    if (scoped->active && scoped->active->loop && scoped->active->sm) {
        auto entry = scoped->active;
        auto receipt = entry->loop->enqueue_control([entry, title]() {
            if (!entry->sm) return false;
            entry->sm->set_session_title(title);
            return true;
        });
        if (!receipt.accepted ||
            !receipt.wait_for_completion(std::chrono::seconds(5)) ||
            !receipt.succeeded()) {
            return SessionControlResult::fail("title update could not be applied");
        }
    } else {
        scoped->meta.title = title;
        scoped->meta.title_source = title.empty() ? "user-cleared" : "user";
        if (!SessionStorage::write_meta(
                SessionStorage::meta_path(scoped->project_dir, session_id),
                scoped->meta)) {
            return SessionControlResult::fail("failed to persist title");
        }
    }
    return SessionControlResult::ok(json{{"session_id", session_id}, {"title", title}});
}

SessionControlResult SessionControlService::set_archived(
    const SessionControlScope& scope,
    const std::string& session_id,
    bool archived) const {
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped) return SessionControlResult::fail("session is unavailable in this workspace scope");
    if (archived && scoped->active && deps_.client) {
        deps_.client->destroy_session(session_id);
        scoped = resolve_scoped_session(deps_, scope, session_id);
        if (!scoped) return SessionControlResult::fail("session disappeared while archiving");
    }
    if (!archived && scoped->active && scoped->active->sm) {
        scoped->active->sm->set_session_archived(false);
    }
    scoped->meta.archived = archived;
    if (!SessionStorage::write_meta(
            SessionStorage::meta_path(scoped->project_dir, session_id),
            scoped->meta)) {
        return SessionControlResult::fail("failed to persist archive state");
    }
    if (archived) {
        const auto path = path_from_utf8(scoped->project_dir) /
            "pinned_sessions.json";
        auto pins = session_pins::read_pinned_sessions_state(path);
        pins.session_ids = session_pins::unpin_session_id(
            pins.session_ids, session_id);
        std::string ignored;
        session_pins::write_pinned_sessions_state(path, pins, &ignored);
    }
    return SessionControlResult::ok(json{
        {"session_id", session_id}, {"archived", archived},
    });
}

SessionControlResult SessionControlService::set_pinned(
    const SessionControlScope& scope,
    const std::string& session_id,
    bool pinned) const {
    auto scoped = resolve_scoped_session(deps_, scope, session_id);
    if (!scoped || scoped->meta.archived) {
        return SessionControlResult::fail("session is unavailable for pinning in this workspace scope");
    }
    const auto path = path_from_utf8(scoped->project_dir) /
        "pinned_sessions.json";
    auto state = session_pins::read_pinned_sessions_state(path);
    state.session_ids = pinned
        ? session_pins::pin_session_id(state.session_ids, session_id)
        : session_pins::unpin_session_id(state.session_ids, session_id);
    std::string error;
    if (!session_pins::write_pinned_sessions_state(path, state, &error)) {
        return SessionControlResult::fail("failed to persist pin state: " + error);
    }
    return SessionControlResult::ok(json{
        {"session_id", session_id}, {"pinned", pinned},
    });
}

} // namespace acecode
