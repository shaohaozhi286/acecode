#pragma once

#include "../provider/llm_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace acecode {

struct RewindTarget {
    size_t message_index = 0;
    std::string message_uuid;
    std::string preview;
    bool has_stable_uuid = false;
};

// Assign identity fields needed by /rewind. No-op for non-user messages and
// for messages that already carry ids from a resumed session.
void ensure_user_message_identity(ChatMessage& msg);

// True only for user-authored turns that make sense as rewind boundaries.
bool is_rewind_selectable_user_message(const ChatMessage& msg);

// File checkpoint meta messages are persisted in JSONL but should not render as
// normal chat rows and should not be considered conversation content.
bool is_file_checkpoint_message(const ChatMessage& msg);

std::vector<RewindTarget> collect_rewind_targets(const std::vector<ChatMessage>& messages);

std::vector<ChatMessage> retained_prefix_before_index(
    const std::vector<ChatMessage>& messages,
    size_t target_index);

std::string rewind_prefill_text(const ChatMessage& msg);

std::string rewind_preview_text(const ChatMessage& msg, size_t max_bytes = 80);

} // namespace acecode
