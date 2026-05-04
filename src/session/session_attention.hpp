#pragma once

#include "session_client.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

enum class SessionAttentionState {
    Read,
    Unread,
    InProgress,
};

struct SessionAttentionRecord {
    bool busy = false;
    std::uint64_t update_cursor = 0;
    std::uint64_t read_cursor = 0;
    std::int64_t updated_at_ms = 0;
};

const char* to_string(SessionAttentionState state);
std::optional<SessionAttentionState> parse_session_attention_state(const std::string& value);

SessionAttentionState session_attention_state_for(const SessionAttentionRecord& record);

bool session_event_has_user_visible_output(SessionEventKind kind,
                                           const nlohmann::json& payload);

SessionAttentionRecord apply_session_attention_event(
    SessionAttentionRecord record,
    SessionEventKind kind,
    const nlohmann::json& payload,
    std::uint64_t cursor,
    std::int64_t timestamp_ms);

SessionAttentionRecord mark_session_attention_read(
    SessionAttentionRecord record,
    std::uint64_t cursor,
    std::int64_t timestamp_ms);

} // namespace acecode
