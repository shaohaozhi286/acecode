#include "session_attention.hpp"

#include <algorithm>

namespace acecode {

const char* to_string(SessionAttentionState state) {
    switch (state) {
        case SessionAttentionState::Read:       return "read";
        case SessionAttentionState::Unread:     return "unread";
        case SessionAttentionState::InProgress: return "in_progress";
    }
    return "read";
}

std::optional<SessionAttentionState> parse_session_attention_state(const std::string& value) {
    if (value == "read") return SessionAttentionState::Read;
    if (value == "unread") return SessionAttentionState::Unread;
    if (value == "in_progress") return SessionAttentionState::InProgress;
    return std::nullopt;
}

SessionAttentionState session_attention_state_for(const SessionAttentionRecord& record) {
    if (record.busy) return SessionAttentionState::InProgress;
    return record.update_cursor > record.read_cursor
        ? SessionAttentionState::Unread
        : SessionAttentionState::Read;
}

bool session_event_has_user_visible_output(SessionEventKind kind,
                                           const nlohmann::json& payload) {
    switch (kind) {
        case SessionEventKind::Token:
        case SessionEventKind::Reasoning:
        case SessionEventKind::ToolStart:
        case SessionEventKind::ToolUpdate:
        case SessionEventKind::ToolEnd:
        case SessionEventKind::PermissionRequest:
        case SessionEventKind::QuestionRequest:
        case SessionEventKind::Error:
            return true;
        case SessionEventKind::Message: {
            const auto role = payload.value("role", std::string{});
            return role != "user";
        }
        case SessionEventKind::Usage:
        case SessionEventKind::BusyChanged:
        case SessionEventKind::Done:
            return false;
    }
    return false;
}

SessionAttentionRecord apply_session_attention_event(
    SessionAttentionRecord record,
    SessionEventKind kind,
    const nlohmann::json& payload,
    std::uint64_t cursor,
    std::int64_t timestamp_ms) {
    if (kind == SessionEventKind::BusyChanged && payload.is_object() && payload.contains("busy")) {
        record.busy = payload.value("busy", false);
        record.updated_at_ms = std::max(record.updated_at_ms, timestamp_ms);
    }

    if (session_event_has_user_visible_output(kind, payload)) {
        record.update_cursor = std::max(record.update_cursor, cursor);
        record.updated_at_ms = std::max(record.updated_at_ms, timestamp_ms);
    }

    return record;
}

SessionAttentionRecord mark_session_attention_read(
    SessionAttentionRecord record,
    std::uint64_t cursor,
    std::int64_t timestamp_ms) {
    const std::uint64_t effective_cursor = cursor == 0 ? record.update_cursor : cursor;
    record.read_cursor = std::max(record.read_cursor, effective_cursor);
    record.updated_at_ms = std::max(record.updated_at_ms, timestamp_ms);
    return record;
}

} // namespace acecode
