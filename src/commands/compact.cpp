#include "compact.hpp"
#include "compact_prompt.hpp"
#include "../agent_loop.hpp"
#include "../session/compact_checkpoint.hpp"
#include "../utils/logger.hpp"
#include "../utils/uuid.hpp"

#include <algorithm>
#include <cctype>
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

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_any(const std::string& haystack, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string provider_error_search_text(const acecode::ProviderErrorInfo& info) {
    return ascii_lower(info.display_message + "\n" + info.raw_body + "\n" + info.pretty_json);
}

std::string compact_trigger_name(bool is_auto) {
    return is_auto ? "auto" : "manual";
}

int find_tail_start_for_user_turns(const std::vector<acecode::ChatMessage>& messages, int user_turns) {
    const int target_turns = std::max(1, user_turns);
    int turns_found = 0;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (!messages[i].is_meta && messages[i].role == "user") {
            ++turns_found;
            if (turns_found >= target_turns) {
                return i;
            }
        }
    }
    return 0;
}

std::vector<int> rescue_tail_candidates(int preferred_tail_user_turns) {
    std::vector<int> candidates;
    auto add = [&candidates](int turns) {
        if (turns <= 0) return;
        if (std::find(candidates.begin(), candidates.end(), turns) == candidates.end()) {
            candidates.push_back(turns);
        }
    };
    add(preferred_tail_user_turns);
    add(4);
    add(2);
    add(1);
    return candidates;
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
    return provider_relevant_messages(messages);
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
    if (groups.empty()) {
        LOG_WARN("Compact PTL retry cannot truncate because no API round groups were found; messages=" +
                 std::to_string(messages.size()));
        return messages;
    }

    // Always keep at least 1 group
    int drop = std::min(groups_to_drop, static_cast<int>(groups.size()) - 1);
    if (drop <= 0 && groups.size() > 1) {
        // Default: drop ~20% of groups
        drop = std::max(1, static_cast<int>(groups.size()) / 5);
    }
    if (drop <= 0) {
        LOG_WARN("Compact PTL retry cannot truncate without dropping the only remaining API round; groups=" +
                 std::to_string(groups.size()) +
                 " messages=" + std::to_string(messages.size()));
        return messages;
    }

    int start_from = groups[drop].start_index;
    std::vector<ChatMessage> result(messages.begin() + start_from, messages.end());

    // Ensure the truncated list starts with a valid user message
    if (!result.empty() && result.front().role != "user") {
        ChatMessage synthetic;
        synthetic.role = "user";
        synthetic.content = "[Previous conversation was truncated for context length. Continuing from here.]";
        result.insert(result.begin(), std::move(synthetic));
    }

    LOG_WARN("Compact PTL retry truncated oldest API rounds; groups_before=" +
             std::to_string(groups.size()) +
             " groups_dropped=" + std::to_string(drop) +
             " messages_before=" + std::to_string(messages.size()) +
             " messages_after=" + std::to_string(result.size()) +
             " estimated_tokens_before=" + std::to_string(estimate_message_tokens(messages)) +
             " estimated_tokens_after=" + std::to_string(estimate_message_tokens(result)));
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
    const auto provider_messages = provider_relevant_messages(messages);

    // A long sequence of small assistant/tool messages can remain below the
    // token threshold while still overwhelming provider/model message
    // handling. Never let token usage mask that structural pressure.
    if (provider_messages.size() >
        static_cast<std::size_t>(AUTOCOMPACT_MAX_PROVIDER_MESSAGES)) {
        return true;
    }

    // Prefer API-reported prompt_tokens when available (accurate)
    if (last_api_prompt_tokens > 0) {
        return last_api_prompt_tokens > threshold;
    }
    // Fallback: estimate from provider-visible content (chars/4 heuristic).
    int estimated = estimate_message_tokens(provider_messages);
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
// Context-overflow rescue compact
// ============================================================

bool is_context_overflow_error(const ProviderErrorInfo& info) {
    if (info.status_code == 413) {
        return true;
    }

    const std::string text = provider_error_search_text(info);
    static const std::vector<std::string> overflow_needles = {
        "context_length_exceeded",
        "maximum context length",
        "max context length",
        "context length",
        "context window",
        "context limit",
        "context is too long",
        "context too long",
        "prompt is too long",
        "prompt too long",
        "token limit",
        "too many tokens",
        "tokens in the messages",
        "input is too long",
        "input too long",
        "input tokens",
        "input too large",
        "request too large",
        "payload too large",
        "reduce the length of the messages",
        "exceeds the model",
        "exceeded model"
    };
    return contains_any(text, overflow_needles);
}

bool should_attempt_context_overflow_rescue(
    const ProviderErrorInfo& info,
    int estimated_request_tokens,
    int context_window,
    bool model_output_seen
) {
    if (model_output_seen) {
        return false;
    }
    if (is_context_overflow_error(info)) {
        return true;
    }
    if (info.kind != ProviderErrorKind::Http ||
        (info.status_code != 400 && info.status_code != 422)) {
        return false;
    }

    const std::string text = provider_error_search_text(info);
    const bool ambiguous_bad_request =
        text.empty() ||
        text.find("bad request") != std::string::npos ||
        text.find("invalid_request") != std::string::npos ||
        text.find("invalid request") != std::string::npos;
    if (!ambiguous_bad_request) {
        return false;
    }

    const int auto_threshold = get_auto_compact_threshold(context_window);
    int fallback_threshold = 32000;
    if (auto_threshold > 0) {
        // Ambiguous 400s from OpenAI-compatible gateways often hide the real
        // context limit. Do not trust an optimistic catalog/config window here:
        // if the request is already above a 32k-class size, rescue is safer
        // than replaying the same oversized history forever.
        fallback_threshold = std::min(auto_threshold, fallback_threshold);
    } else if (context_window > 0) {
        fallback_threshold = std::min(context_window, fallback_threshold);
    }
    return estimated_request_tokens > fallback_threshold;
}

ContextRescueResult rescue_compact_messages(
    const std::vector<ChatMessage>& messages,
    const std::string& cwd,
    int preferred_tail_user_turns
) {
    ContextRescueResult result;
    result.estimated_tokens_before = estimate_message_tokens(messages);

    LOG_WARN("Rescue compact start; messages=" + std::to_string(messages.size()) +
             " estimated_tokens_before=" + std::to_string(result.estimated_tokens_before) +
             " preferred_tail_user_turns=" + std::to_string(preferred_tail_user_turns) +
             " cwd=" + log_truncate(cwd, 200));

    if (messages.empty()) {
        result.error = "No conversation history to rescue compact.";
        LOG_WARN("Rescue compact aborted: " + result.error);
        return result;
    }

    for (int tail_turns : rescue_tail_candidates(preferred_tail_user_turns)) {
        const int tail_start = find_tail_start_for_user_turns(messages, tail_turns);
        LOG_WARN("Rescue compact candidate; protected_user_turns=" +
                 std::to_string(tail_turns) +
                 " tail_start=" + std::to_string(tail_start) +
                 " removable_prefix_messages=" + std::to_string(std::max(0, tail_start)));
        if (tail_start <= 0) {
            LOG_WARN("Rescue compact candidate rejected; no removable prefix for protected_user_turns=" +
                     std::to_string(tail_turns));
            continue;
        }

        std::vector<ChatMessage> compacted;
        compacted.reserve(static_cast<std::size_t>(messages.size() - tail_start + 3));
        compacted.push_back(create_compact_boundary_message("rescue", result.estimated_tokens_before));

        ChatMessage summary_msg;
        summary_msg.role = "system";
        summary_msg.is_compact_summary = true;
        summary_msg.content =
            "[Conversation summary - earlier context truncated because the provider rejected the request as too large.]\n\n"
            "Earlier conversation context was removed by rescue compact because the provider rejected the previous "
            "request as too large. A detailed summary was not generated. Continue from the preserved recent "
            "messages below and the current working directory.";
        summary_msg.metadata = {
            {"trigger", "rescue"},
            {"summary_generated", false},
            {"protected_user_turns", tail_turns}
        };
        compacted.push_back(summary_msg);

        ChatMessage cwd_msg;
        cwd_msg.role = "system";
        cwd_msg.content = "[Post-compact context] Current working directory: " + cwd;
        cwd_msg.is_meta = true;
        compacted.push_back(cwd_msg);

        compacted.insert(compacted.end(), messages.begin() + tail_start, messages.end());

        const int after = estimate_message_tokens(compacted);
        if (after >= result.estimated_tokens_before) {
            LOG_WARN("Rescue compact candidate rejected; estimated tokens did not shrink" +
                     std::string(" before=") + std::to_string(result.estimated_tokens_before) +
                     " after=" + std::to_string(after) +
                     " protected_user_turns=" + std::to_string(tail_turns));
            continue;
        }

        result.performed = true;
        result.can_retry = true;
        result.messages_removed = tail_start;
        result.estimated_tokens_after = after;
        result.protected_user_turns = tail_turns;
        result.marker_text = summary_msg.content;
        result.compacted_messages = std::move(compacted);
        LOG_WARN("Rescue compact accepted; messages_removed=" +
                 std::to_string(result.messages_removed) +
                 " messages_after=" + std::to_string(result.compacted_messages.size()) +
                 " estimated_tokens_before=" + std::to_string(result.estimated_tokens_before) +
                 " estimated_tokens_after=" + std::to_string(result.estimated_tokens_after) +
                 " protected_user_turns=" + std::to_string(result.protected_user_turns));
        return result;
    }

    result.error = "Current request is too large to rescue by compacting earlier history.";
    result.estimated_tokens_after = result.estimated_tokens_before;
    LOG_WARN("Rescue compact failed; " + result.error +
             " messages=" + std::to_string(messages.size()) +
             " estimated_tokens=" + std::to_string(result.estimated_tokens_before));
    return result;
}

// ============================================================
// Full Compact (tasks 5.x + 6.x PTL retry)
// ============================================================

CompactResult compact_messages(
    LlmProvider& provider,
    const std::vector<ChatMessage>& messages,
    const std::string& cwd,
    int keep_turns,
    bool is_auto,
    std::atomic<bool>* abort_flag
) {
    CompactResult result;

    LOG_INFO("Compact start; trigger=" + compact_trigger_name(is_auto) +
             " messages=" + std::to_string(messages.size()) +
             " keep_turns=" + std::to_string(keep_turns) +
             " cwd=" + log_truncate(cwd, 200));

    if (messages.empty()) {
        result.error = "No conversation history to compact.";
        LOG_WARN("Compact aborted; trigger=" + compact_trigger_name(is_auto) +
                 " error=" + result.error);
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
        LOG_INFO("Compact skipped; trigger=" + compact_trigger_name(is_auto) +
                 " reason=not_enough_history" +
                 " active_start=" + std::to_string(active_start) +
                 " keep_from=" + std::to_string(keep_from) +
                 " total_messages=" + std::to_string(messages.size()));
        return result;
    }

    // Collect messages to summarize (between active_start and keep_from)
    std::vector<ChatMessage> to_summarize(messages.begin() + active_start, messages.begin() + keep_from);

    int tokens_before = estimate_message_tokens(to_summarize);
    if (tokens_before < kMinimumTokensToCompact) {
        result.error = "Not enough conversation history to compact.";
        LOG_INFO("Compact skipped; trigger=" + compact_trigger_name(is_auto) +
                 " reason=below_minimum_tokens" +
                 " tokens_to_summarize=" + std::to_string(tokens_before) +
                 " minimum_tokens=" + std::to_string(kMinimumTokensToCompact) +
                 " messages_to_summarize=" + std::to_string(to_summarize.size()));
        return result;
    }

    LOG_INFO("Compact summarization prepared; trigger=" + compact_trigger_name(is_auto) +
             " boundary_start=" + std::to_string(boundary_start) +
             " active_start=" + std::to_string(active_start) +
             " keep_from=" + std::to_string(keep_from) +
             " messages_to_summarize=" + std::to_string(to_summarize.size()) +
             " messages_to_keep=" + std::to_string(messages.size() - keep_from) +
             " estimated_tokens_to_summarize=" + std::to_string(tokens_before));

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
            LOG_WARN("Compact cancelled before summarization call; trigger=" +
                     compact_trigger_name(is_auto) +
                     " attempt=" + std::to_string(attempt + 1));
            return result;
        }

        try {
            LOG_INFO("Compact summarization call; trigger=" + compact_trigger_name(is_auto) +
                     " attempt=" + std::to_string(attempt + 1) +
                     " of=" + std::to_string(MAX_PTL_RETRIES + 1) +
                     " summarize_messages=" + std::to_string(summarize_input.size()) +
                     " estimated_tokens=" + std::to_string(estimate_message_tokens(summarize_input)) +
                     " request_messages=" + std::to_string(summary_messages.size()));
            ChatResponse resp = provider.chat(summary_messages, {});

            // Check abort after the LLM call returns
            if (abort_flag && abort_flag->load()) {
                result.error = "Compaction cancelled.";
                LOG_WARN("Compact cancelled after summarization call; trigger=" +
                         compact_trigger_name(is_auto) +
                         " attempt=" + std::to_string(attempt + 1));
                return result;
            }

            if (resp.finish_reason == "error" && is_ptl_error(resp.content)) {
                LOG_WARN("Compact PTL error; trigger=" + compact_trigger_name(is_auto) +
                         " attempt=" + std::to_string(attempt + 1) +
                         " summarize_messages=" + std::to_string(summarize_input.size()) +
                         " estimated_tokens=" + std::to_string(estimate_message_tokens(summarize_input)) +
                         " error=" + log_truncate(resp.content, 500));
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
                LOG_ERROR("Compact summarization failed; trigger=" + compact_trigger_name(is_auto) +
                          " attempt=" + std::to_string(attempt + 1) +
                          " error=" + log_truncate(resp.content, 500));
                return result;
            }

            summary_text = resp.content;
            if (summary_text.empty()) {
                result.error = "Summarization returned empty response.";
                LOG_ERROR("Compact summarization returned empty response; trigger=" +
                          compact_trigger_name(is_auto) +
                          " attempt=" + std::to_string(attempt + 1));
                return result;
            }
            LOG_INFO("Compact summarization succeeded; trigger=" + compact_trigger_name(is_auto) +
                     " attempt=" + std::to_string(attempt + 1) +
                     " summary_bytes=" + std::to_string(summary_text.size()));
            ptl_success = true;
            break;

        } catch (const std::exception& e) {
            std::string err_msg = e.what();
            if (is_ptl_error(err_msg) && attempt < MAX_PTL_RETRIES) {
                LOG_WARN("Compact PTL exception; trigger=" + compact_trigger_name(is_auto) +
                         " attempt=" + std::to_string(attempt + 1) +
                         " summarize_messages=" + std::to_string(summarize_input.size()) +
                         " estimated_tokens=" + std::to_string(estimate_message_tokens(summarize_input)) +
                         " error=" + log_truncate(err_msg, 500));
                summarize_input = truncate_head_for_ptl_retry(summarize_input);
                continue;
            }
            LOG_ERROR("Compact summarization threw; trigger=" + compact_trigger_name(is_auto) +
                      " attempt=" + std::to_string(attempt + 1) +
                      " error=" + log_truncate(err_msg, 500));
            result.error = "Summarization failed: " + err_msg;
            return result;
        }
    }

    if (!ptl_success) {
        result.error = "Compaction failed after PTL retries.";
        LOG_ERROR("Compact failed after PTL retries; trigger=" + compact_trigger_name(is_auto) +
                  " max_retries=" + std::to_string(MAX_PTL_RETRIES));
        return result;
    }

    // Process the summary: extract <summary> block, strip <analysis>
    summary_text = format_compact_summary(summary_text);

    // Build the final summary user message
    std::string final_summary = get_compact_user_summary_message(summary_text);

    // Build the replacement model history:
    // [boundary marker] + [summary msg] + [cwd msg] + [kept messages]
    std::vector<ChatMessage> new_messages;

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
    cwd_msg.content = "[Post-compact context] Current working directory: " + cwd;
    cwd_msg.is_meta = true;
    new_messages.push_back(cwd_msg);

    // Append kept messages (from keep_from to end)
    new_messages.insert(new_messages.end(), messages.begin() + keep_from, messages.end());

    result.performed = true;
    result.messages_compressed = keep_from - active_start;
    result.estimated_tokens_saved = std::max(0, tokens_before - estimate_message_tokens({summary_msg}));
    result.summary_text = summary_text;
    result.compacted_messages = std::move(new_messages);

    LOG_INFO("Compact complete; trigger=" + compact_trigger_name(is_auto) +
             " messages_compressed=" + std::to_string(result.messages_compressed) +
             " messages_before=" + std::to_string(messages.size()) +
             " messages_after=" + std::to_string(result.compacted_messages.size()) +
             " estimated_tokens_saved=" + std::to_string(result.estimated_tokens_saved) +
             " summary_bytes=" + std::to_string(result.summary_text.size()));
    return result;
}

CompactResult compact_context(
    LlmProvider& provider,
    AgentLoop& agent_loop,
    TuiState& state,
    int keep_turns,
    bool is_auto,
    std::atomic<bool>* abort_flag
) {
    CompactResult result = compact_messages(
        provider,
        agent_loop.messages(),
        agent_loop.cwd(),
        keep_turns,
        is_auto,
        abort_flag);

    if (!result.performed) {
        return result;
    }

    agent_loop.messages_mut() = provider_relevant_messages(result.compacted_messages);

    // Append a visible marker without removing earlier human transcript rows.
    {
        std::lock_guard<std::mutex> lk(state.mu);
        state.conversation.push_back({"system", "--- [Compact Checkpoint] ---", false});
        state.conversation.push_back({"system", "[Conversation summary]\n" + result.summary_text, false});
        state.chat_follow_tail = true;
    }

    return result;
}

} // namespace acecode
