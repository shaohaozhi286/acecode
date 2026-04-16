#include "compact.hpp"
#include "compact_prompt.hpp"
#include "../agent_loop.hpp"
#include "../utils/logger.hpp"
#include "../utils/uuid.hpp"

#include <algorithm>
#include <stdexcept>
#include <regex>
#include <sstream>

namespace {

constexpr int kMinimumTokensToCompact = 200;

// Check if an API error indicates prompt-too-long
bool is_ptl_error(const std::string& error_msg) {
    // Common patterns from OpenAI-compatible APIs
    return error_msg.find("maximum context length") != std::string::npos
        || error_msg.find("prompt is too long") != std::string::npos
        || error_msg.find("token limit") != std::string::npos
        || error_msg.find("context_length_exceeded") != std::string::npos;
}

} // namespace

namespace acecode {

// ============================================================
// Token estimation
// ============================================================

int estimate_message_tokens(const std::vector<ChatMessage>& messages) {
    int total_chars = 0;
    for (const auto& msg : messages) {
        total_chars += static_cast<int>(msg.content.size());
        if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            total_chars += static_cast<int>(msg.tool_calls.dump().size());
        }
    }
    return total_chars / 4; // rough estimate: ~4 chars per token
}

// ============================================================
// Compact Boundary (tasks 2.1-2.4)
// ============================================================

ChatMessage create_compact_boundary_message(const std::string& trigger, int pre_tokens) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[Compact boundary]";
    msg.uuid = generate_uuid();
    msg.subtype = "compact_boundary";
    msg.timestamp = iso_timestamp();
    msg.is_meta = true;
    msg.metadata = {
        {"trigger", trigger},
        {"pre_tokens", pre_tokens}
    };
    return msg;
}

bool is_compact_boundary_message(const ChatMessage& msg) {
    return msg.subtype == "compact_boundary";
}

int find_last_compact_boundary_index(const std::vector<ChatMessage>& messages) {
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (is_compact_boundary_message(messages[i])) {
            return i;
        }
    }
    return -1;
}

std::pair<int, int> get_messages_after_compact_boundary(const std::vector<ChatMessage>& messages) {
    int idx = find_last_compact_boundary_index(messages);
    if (idx < 0) {
        return {0, static_cast<int>(messages.size())};
    }
    return {idx, static_cast<int>(messages.size()) - idx};
}

// ============================================================
// API message filtering (task 2.5)
// ============================================================

std::vector<ChatMessage> normalize_messages_for_api(const std::vector<ChatMessage>& messages) {
    std::vector<ChatMessage> result;
    result.reserve(messages.size());
    for (const auto& msg : messages) {
        if (!msg.is_meta) {
            result.push_back(msg);
        }
    }
    return result;
}

// ============================================================
// PTL retry helpers (tasks 6.1-6.4)
// ============================================================

std::vector<ApiRoundGroup> group_messages_by_api_round(const std::vector<ChatMessage>& messages) {
    std::vector<ApiRoundGroup> groups;
    int i = 0;
    int n = static_cast<int>(messages.size());

    while (i < n) {
        // Skip system/meta messages between rounds
        if (messages[i].role == "system" || messages[i].is_meta) {
            ++i;
            continue;
        }

        ApiRoundGroup group;
        group.start_index = i;

        // A round starts with a user message
        if (messages[i].role == "user") {
            ++i;
            // Include assistant response
            while (i < n && messages[i].role == "assistant") {
                ++i;
            }
            // Include tool results following the assistant
            while (i < n && messages[i].role == "tool") {
                ++i;
            }
        } else {
            // Non-user start (shouldn't normally happen) - include single message
            ++i;
        }

        group.end_index = i;
        groups.push_back(group);
    }

    return groups;
}

std::vector<ChatMessage> truncate_head_for_ptl_retry(
    const std::vector<ChatMessage>& messages,
    int groups_to_drop
) {
    auto groups = group_messages_by_api_round(messages);
    if (groups.empty()) return messages;

    // Always keep at least 1 group
    int drop = std::min(groups_to_drop, static_cast<int>(groups.size()) - 1);
    if (drop <= 0 && groups.size() > 1) {
        // Default: drop ~20% of groups
        drop = std::max(1, static_cast<int>(groups.size()) / 5);
    }
    if (drop <= 0) return messages;

    int start_from = groups[drop].start_index;
    std::vector<ChatMessage> result(messages.begin() + start_from, messages.end());

    // Ensure the truncated list starts with a valid user message
    if (!result.empty() && result.front().role != "user") {
        ChatMessage synthetic;
        synthetic.role = "user";
        synthetic.content = "[Previous conversation was truncated for context length. Continuing from here.]";
        result.insert(result.begin(), std::move(synthetic));
    }

    return result;
}

// ============================================================
// Auto-compact threshold (tasks 7.x)
// ============================================================

int get_effective_context_window(int context_window) {
    return context_window - MAX_OUTPUT_TOKENS_RESERVED;
}

int get_auto_compact_threshold(int context_window) {
    int threshold = get_effective_context_window(context_window) - AUTOCOMPACT_BUFFER_TOKENS;
    return std::max(0, threshold);
}

bool should_auto_compact(const std::vector<ChatMessage>& messages, int context_window, int last_api_prompt_tokens) {
    int threshold = get_auto_compact_threshold(context_window);
    // Prefer API-reported prompt_tokens when available (accurate)
    if (last_api_prompt_tokens > 0) {
        return last_api_prompt_tokens > threshold;
    }
    // Fallback: estimate from message content (chars/4 heuristic)
    auto [start, count] = get_messages_after_compact_boundary(messages);
    std::vector<ChatMessage> active(messages.begin() + start, messages.begin() + start + count);
    int estimated = estimate_message_tokens(active);
    return estimated > threshold;
}

TokenWarningState calculate_token_warning_state(int estimated_tokens, int context_window) {
    TokenWarningState state;
    int effective = get_effective_context_window(context_window);
    if (effective <= 0) {
        state.percent_left = 0.0;
        state.is_above_warning = true;
        state.is_above_error = true;
        state.is_above_auto_compact = true;
        return state;
    }

    int remaining = effective - estimated_tokens;
    state.percent_left = (static_cast<double>(remaining) / effective) * 100.0;
    state.is_above_warning = remaining < 20000;
    state.is_above_error = remaining < 5000;
    state.is_above_auto_compact = estimated_tokens > get_auto_compact_threshold(context_window);
    return state;
}

// ============================================================
// Full Compact (tasks 5.x + 6.x PTL retry)
// ============================================================

CompactResult compact_context(
    LlmProvider& provider,
    AgentLoop& agent_loop,
    TuiState& state,
    int keep_turns,
    bool is_auto,
    std::atomic<bool>* abort_flag
) {
    CompactResult result;
    auto& messages = agent_loop.messages_mut();

    if (messages.empty()) {
        result.error = "No conversation history to compact.";
        return result;
    }

    // Get messages after the last compact boundary.
    // When no boundary exists, summarize from the beginning.
    const int last_boundary_index = find_last_compact_boundary_index(messages);
    const int boundary_start = last_boundary_index >= 0 ? last_boundary_index : 0;
    const int active_start = last_boundary_index >= 0 ? last_boundary_index + 1 : 0;

    // Count user/assistant turn pairs from the end to find the keep boundary
    int turns_found = 0;
    int keep_from = static_cast<int>(messages.size());

    for (int i = static_cast<int>(messages.size()) - 1; i >= active_start; --i) {
        if (messages[i].role == "user") {
            turns_found++;
            if (turns_found >= keep_turns) {
                keep_from = i;
                break;
            }
        }
    }

    // Need at least some messages to compress after the boundary
    if (keep_from <= active_start) {
        result.error = "Not enough conversation history to compact.";
        return result;
    }

    // Collect messages to summarize (between active_start and keep_from)
    std::vector<ChatMessage> to_summarize(messages.begin() + active_start, messages.begin() + keep_from);

    int tokens_before = estimate_message_tokens(to_summarize);
    if (tokens_before < kMinimumTokensToCompact) {
        result.error = "Not enough conversation history to compact.";
        return result;
    }

    // Build the structured compact prompt
    std::string compact_prompt = get_compact_prompt(is_auto);
    std::string messages_content;
    for (const auto& msg : to_summarize) {
        if (msg.is_meta) continue;
        messages_content += "[" + msg.role + "]: " + msg.content + "\n";
        if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            messages_content += "[tool_calls]: " + msg.tool_calls.dump() + "\n";
        }
    }

    // PTL retry loop
    std::string summary_text;
    std::vector<ChatMessage> summarize_input = to_summarize;
    bool ptl_success = false;

    for (int attempt = 0; attempt <= MAX_PTL_RETRIES; ++attempt) {
        // Build summarization request
        std::string input_content;
        for (const auto& msg : summarize_input) {
            if (msg.is_meta) continue;
            input_content += "[" + msg.role + "]: " + msg.content + "\n";
            if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
                input_content += "[tool_calls]: " + msg.tool_calls.dump() + "\n";
            }
        }

        std::vector<ChatMessage> summary_messages;
        ChatMessage sys_msg;
        sys_msg.role = "system";
        sys_msg.content = compact_prompt;
        summary_messages.push_back(std::move(sys_msg));

        ChatMessage user_msg;
        user_msg.role = "user";
        user_msg.content = "Here is the conversation to summarize:\n\n" + input_content;
        summary_messages.push_back(std::move(user_msg));

        // Check abort before the (potentially long) LLM call
        if (abort_flag && abort_flag->load()) {
            result.error = "Compaction cancelled.";
            return result;
        }

        try {
            ChatResponse resp = provider.chat(summary_messages, {});

            // Check abort after the LLM call returns
            if (abort_flag && abort_flag->load()) {
                result.error = "Compaction cancelled.";
                return result;
            }

            if (resp.finish_reason == "error" && is_ptl_error(resp.content)) {
                LOG_WARN("Compact: PTL error on attempt " + std::to_string(attempt + 1));
                if (attempt < MAX_PTL_RETRIES) {
                    summarize_input = truncate_head_for_ptl_retry(summarize_input);
                    continue;
                }
                result.error = "Conversation too long for compaction after " +
                               std::to_string(MAX_PTL_RETRIES) + " retries.";
                return result;
            }

            if (resp.finish_reason == "error") {
                result.error = "Summarization failed: " + resp.content;
                return result;
            }

            summary_text = resp.content;
            if (summary_text.empty()) {
                result.error = "Summarization returned empty response.";
                return result;
            }
            ptl_success = true;
            break;

        } catch (const std::exception& e) {
            std::string err_msg = e.what();
            if (is_ptl_error(err_msg) && attempt < MAX_PTL_RETRIES) {
                LOG_WARN("Compact: PTL exception on attempt " + std::to_string(attempt + 1));
                summarize_input = truncate_head_for_ptl_retry(summarize_input);
                continue;
            }
            LOG_ERROR("Compact: summarization failed: " + err_msg);
            result.error = "Summarization failed: " + err_msg;
            return result;
        }
    }

    if (!ptl_success) {
        result.error = "Compaction failed after PTL retries.";
        return result;
    }

    // Process the summary: extract <summary> block, strip <analysis>
    summary_text = format_compact_summary(summary_text);

    // Build the final summary user message
    std::string final_summary = get_compact_user_summary_message(summary_text);

    // Build new message list:
    // [messages before boundary] + [boundary marker] + [summary msg] + [cwd msg] + [kept messages]
    std::vector<ChatMessage> new_messages;

    // Preserve everything before the active region (including old boundaries/summaries)
    for (int i = 0; i < boundary_start; ++i) {
        new_messages.push_back(messages[i]);
    }
    // If old boundary existed, preserve it too
    if (last_boundary_index >= 0) {
        new_messages.push_back(messages[last_boundary_index]);
    }

    // Insert new compact boundary
    std::string trigger = is_auto ? "auto" : "manual";
    ChatMessage boundary = create_compact_boundary_message(trigger, tokens_before);
    new_messages.push_back(boundary);

    // Insert summary message
    ChatMessage summary_msg;
    summary_msg.role = "system";
    summary_msg.content = final_summary;
    summary_msg.is_compact_summary = true;
    new_messages.push_back(summary_msg);

    // Post-compact state re-injection: current working directory
    ChatMessage cwd_msg;
    cwd_msg.role = "system";
    cwd_msg.content = "[Post-compact context] Current working directory: " + agent_loop.cwd();
    cwd_msg.is_meta = true;
    new_messages.push_back(cwd_msg);

    // Append kept messages (from keep_from to end)
    new_messages.insert(new_messages.end(), messages.begin() + keep_from, messages.end());

    result.performed = true;
    result.messages_compressed = keep_from - active_start;
    result.estimated_tokens_saved = std::max(0, tokens_before - estimate_message_tokens({summary_msg}));

    // Update agent loop messages
    messages = std::move(new_messages);

    // Update TUI conversation display
    {
        std::lock_guard<std::mutex> lk(state.mu);
        int tui_keep = 0;
        int tui_turns = 0;
        for (int i = static_cast<int>(state.conversation.size()) - 1; i >= 0; --i) {
            if (state.conversation[i].role == "user") {
                tui_turns++;
                if (tui_turns >= keep_turns) {
                    tui_keep = i;
                    break;
                }
            }
        }
        std::vector<TuiState::Message> new_conv;
        new_conv.reserve(state.conversation.size() - tui_keep + 2);
        new_conv.push_back({"system", "--- [Compact Boundary] ---", false});
        new_conv.push_back({"system", "[Conversation summary]\n" + summary_text, false});
        new_conv.insert(new_conv.end(),
                        state.conversation.begin() + tui_keep,
                        state.conversation.end());
        state.conversation = std::move(new_conv);
        state.chat_follow_tail = true;
    }

    return result;
}

} // namespace acecode
