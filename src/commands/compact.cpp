#include "compact.hpp"
#include "compact_prompt.hpp"
#include "../session/compact_checkpoint.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <stdexcept>

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_any(const std::string& haystack,
                  const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (haystack.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool is_utf8_continuation(unsigned char value) {
    return (value & 0xC0u) == 0x80u;
}

bool has_internal_user_context_metadata(const acecode::ChatMessage& msg) {
    if (!msg.metadata.is_object()) return false;

    static constexpr const char* kInternalContextKeys[] = {
        "transcript_only",
        "hidden_goal_context",
        "hidden_plan_mode_context",
        "hidden_todo_context",
        "hidden_hook_stop_continuation",
        "compact_initial_context",
    };
    for (const char* key : kInternalContextKeys) {
        if (msg.metadata.value(key, false)) return true;
    }
    return false;
}

std::string truncation_marker(std::size_t removed_tokens) {
    const char* ellipsis = "\xE2\x80\xA6";
    return std::string(ellipsis) + std::to_string(removed_tokens) +
           " tokens truncated" + ellipsis;
}

std::size_t message_payload_bytes(const acecode::ChatMessage& msg) {
    std::size_t bytes = msg.content.size() + msg.reasoning_content.size();
    if (msg.content_parts.is_array() && !msg.content_parts.empty()) {
        bytes += msg.content_parts.dump().size();
    }
    if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
        bytes += msg.tool_calls.dump().size();
    }
    bytes += msg.tool_call_id.size();
    // Account for the role and request envelope without pretending to have a
    // provider-specific tokenizer.
    bytes += msg.role.size() + 16;
    return bytes;
}

std::string provider_error_search_text(const acecode::ProviderErrorInfo& info) {
    return ascii_lower(info.display_message + "\n" + info.raw_body + "\n" +
                       info.pretty_json);
}

std::string compact_trigger_name(bool is_auto) {
    return is_auto ? "auto" : "manual";
}

int remove_oldest_history_item(
    std::vector<acecode::ChatMessage>& history) {
    if (history.empty()) return 0;

    acecode::ChatMessage removed = std::move(history.front());
    history.erase(history.begin());
    int removed_count = 1;

    if (removed.role == "assistant" && removed.tool_calls.is_array()) {
        std::set<std::string> call_ids;
        for (const auto& call : removed.tool_calls) {
            if (call.is_object() && call.contains("id") &&
                call["id"].is_string()) {
                call_ids.insert(call["id"].get<std::string>());
            }
        }
        const auto old_size = history.size();
        history.erase(
            std::remove_if(
                history.begin(), history.end(),
                [&](const acecode::ChatMessage& message) {
                    return message.role == "tool" &&
                           call_ids.find(message.tool_call_id) != call_ids.end();
                }),
            history.end());
        removed_count += static_cast<int>(old_size - history.size());
    } else if (removed.role == "tool" && !removed.tool_call_id.empty()) {
        for (auto it = history.begin(); it != history.end(); ++it) {
            if (it->role != "assistant" || !it->tool_calls.is_array()) continue;

            nlohmann::json retained_calls = nlohmann::json::array();
            bool found = false;
            for (const auto& call : it->tool_calls) {
                const bool matches =
                    call.is_object() && call.contains("id") &&
                    call["id"].is_string() &&
                    call["id"].get<std::string>() == removed.tool_call_id;
                if (matches) {
                    found = true;
                } else {
                    retained_calls.push_back(call);
                }
            }
            if (!found) continue;

            if (retained_calls.empty() && it->content.empty() &&
                it->reasoning_content.empty()) {
                history.erase(it);
                ++removed_count;
            } else {
                it->tool_calls = std::move(retained_calls);
            }
            break;
        }
    }
    return removed_count;
}

} // namespace

namespace acecode {

std::size_t approx_token_count(const std::string& text) {
    constexpr std::size_t kApproxBytesPerToken = 4;
    const std::size_t padded =
        text.size() > std::numeric_limits<std::size_t>::max() -
                          (kApproxBytesPerToken - 1)
        ? std::numeric_limits<std::size_t>::max()
        : text.size() + kApproxBytesPerToken - 1;
    return padded / kApproxBytesPerToken;
}

std::string truncate_text_to_token_budget(const std::string& text,
                                          std::size_t max_tokens) {
    if (text.empty()) return {};

    constexpr std::size_t kApproxBytesPerToken = 4;
    const std::size_t max_bytes = max_tokens >
            std::numeric_limits<std::size_t>::max() / kApproxBytesPerToken
        ? std::numeric_limits<std::size_t>::max()
        : max_tokens * kApproxBytesPerToken;

    if (max_tokens > 0 && text.size() <= max_bytes) return text;
    if (max_bytes == 0) return truncation_marker(approx_token_count(text));

    const std::size_t left_budget = max_bytes / 2;
    const std::size_t right_budget = max_bytes - left_budget;

    std::size_t prefix_end = std::min(left_budget, text.size());
    while (prefix_end > 0 && prefix_end < text.size() &&
           is_utf8_continuation(static_cast<unsigned char>(text[prefix_end]))) {
        --prefix_end;
    }

    std::size_t suffix_start = text.size() > right_budget
        ? text.size() - right_budget
        : 0;
    while (suffix_start < text.size() &&
           is_utf8_continuation(static_cast<unsigned char>(text[suffix_start]))) {
        ++suffix_start;
    }
    if (suffix_start < prefix_end) suffix_start = prefix_end;

    const std::size_t removed_bytes = text.size() > max_bytes
        ? text.size() - max_bytes
        : 0;
    const std::size_t removed_tokens =
        (removed_bytes + kApproxBytesPerToken - 1) / kApproxBytesPerToken;
    return text.substr(0, prefix_end) + truncation_marker(removed_tokens) +
           text.substr(suffix_start);
}

int estimate_message_tokens(const std::vector<ChatMessage>& messages) {
    std::size_t total_bytes = 0;
    for (const auto& msg : messages) {
        const std::size_t bytes = message_payload_bytes(msg);
        if (total_bytes > std::numeric_limits<std::size_t>::max() - bytes) {
            return std::numeric_limits<int>::max();
        }
        total_bytes += bytes;
    }
    const std::size_t tokens = (total_bytes + 3) / 4;
    return tokens > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(tokens);
}

bool is_compact_summary_message(const ChatMessage& msg) {
    const std::string prefix = get_compact_summary_prefix() + "\n";
    return msg.is_compact_summary || msg.content.rfind(prefix, 0) == 0;
}

bool is_real_user_message(const ChatMessage& msg) {
    if (msg.role != "user" || msg.is_meta) return false;
    if (has_internal_user_context_metadata(msg)) return false;
    return !is_compact_summary_message(msg);
}

std::vector<ChatMessage> build_compacted_history(
    const std::vector<ChatMessage>& messages,
    const std::string& summary_text,
    std::size_t max_user_message_tokens) {
    std::vector<ChatMessage> selected;
    std::size_t remaining = max_user_message_tokens;

    if (remaining > 0) {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (!is_real_user_message(*it)) continue;
            if (remaining == 0) break;

            ChatMessage retained = *it;
            const std::size_t tokens = approx_token_count(retained.content);
            if (tokens <= remaining) {
                remaining -= tokens;
            } else {
                retained.content =
                    truncate_text_to_token_budget(retained.content, remaining);
                remaining = 0;
            }

            retained.role = "user";
            retained.content_parts = nlohmann::json::array();
            retained.tool_calls = nlohmann::json();
            retained.tool_call_id.clear();
            retained.reasoning_content.clear();
            retained.subtype.clear();
            retained.is_meta = false;
            retained.is_compact_summary = false;
            selected.push_back(std::move(retained));
        }
        std::reverse(selected.begin(), selected.end());
    }

    ChatMessage summary;
    summary.role = "user";
    summary.content = get_compact_user_summary_message(summary_text);
    summary.is_compact_summary = true;
    summary.metadata = nlohmann::json{{"compact_summary", true}};
    selected.push_back(std::move(summary));
    return selected;
}

std::vector<ChatMessage> normalize_messages_for_api(
    const std::vector<ChatMessage>& messages) {
    return provider_relevant_messages(messages);
}

int get_effective_context_window(int context_window) {
    if (context_window <= 0) return 0;
    const long long effective =
        static_cast<long long>(context_window) *
        EFFECTIVE_CONTEXT_WINDOW_PERCENT / 100;
    return effective > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(effective);
}

int get_auto_compact_threshold(int context_window) {
    if (context_window <= 0) return 0;
    const long long automatic =
        static_cast<long long>(context_window) *
        AUTO_COMPACT_CONTEXT_WINDOW_PERCENT / 100;
    return std::min(
        get_effective_context_window(context_window),
        automatic > std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max()
            : static_cast<int>(automatic));
}

bool should_auto_compact(int context_window,
    int server_total_tokens,
    int current_request_estimated_tokens) {
    const int threshold = get_auto_compact_threshold(context_window);
    if (context_window <= 0) return false;
    return std::max(server_total_tokens, current_request_estimated_tokens) >=
           threshold;
}

TokenWarningState calculate_token_warning_state(int estimated_tokens,
                                                 int context_window) {
    TokenWarningState state;
    const int effective = get_effective_context_window(context_window);
    if (effective <= 0) {
        state.percent_left = 0.0;
        state.is_above_warning = true;
        state.is_above_error = true;
        state.is_above_auto_compact = true;
        return state;
    }

    const int remaining = effective - estimated_tokens;
    state.percent_left =
        static_cast<double>(remaining) / static_cast<double>(effective) * 100.0;
    state.is_above_warning = remaining < 20000;
    state.is_above_error = remaining < 5000;
    state.is_above_auto_compact =
        estimated_tokens >= get_auto_compact_threshold(context_window);
    return state;
}

bool is_context_overflow_error(const std::string& error_message) {
    const std::string text = ascii_lower(error_message);
    static const std::vector<std::string> needles = {
        "context_length_exceeded",
        "maximum context length",
        "max context length",
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
        "exceeded model",
    };
    return contains_any(text, needles);
}

bool is_context_overflow_error(const ProviderErrorInfo& info) {
    return info.status_code == 413 ||
           is_context_overflow_error(provider_error_search_text(info));
}

CompactResult compact_messages(
    LlmProvider& provider,
    const std::vector<ChatMessage>& messages,
    const std::vector<ChatMessage>& initial_context,
    bool is_auto,
    std::atomic<bool>* abort_flag) {
    CompactResult result;
    const std::vector<ChatMessage> original_history =
        normalize_messages_for_api(messages);
    std::vector<ChatMessage> request_history = original_history;

    if (provider.supports_native_compaction()) {
        LOG_WARN("Provider advertises native compaction but the active LlmProvider contract "
                 "did not return a validated native compaction item; using local compaction");
    }

    LOG_INFO("Compact start; trigger=" + compact_trigger_name(is_auto) +
             " history_items=" + std::to_string(original_history.size()) +
             " initial_context_items=" + std::to_string(initial_context.size()));

    std::string summary_suffix;
    for (;;) {
        if (abort_flag && abort_flag->load()) {
            result.error = "Compaction cancelled.";
            return result;
        }

        std::vector<ChatMessage> request;
        request.reserve(initial_context.size() + request_history.size() + 1);
        auto stable_context = normalize_messages_for_api(initial_context);
        request.insert(request.end(), stable_context.begin(), stable_context.end());
        request.insert(request.end(), request_history.begin(), request_history.end());

        ChatMessage prompt;
        prompt.role = "user";
        prompt.content = get_compact_prompt();
        request.push_back(std::move(prompt));

        try {
            LOG_INFO("Compact summarization call; trigger=" +
                     compact_trigger_name(is_auto) +
                     " request_items=" + std::to_string(request.size()) +
                     " removable_history_items=" +
                     std::to_string(request_history.size()) +
                     " removed_items=" +
                     std::to_string(result.compaction_request_items_removed));
            ChatResponse response = provider.chat(request, {});

            if (abort_flag && abort_flag->load()) {
                result.error = "Compaction cancelled.";
                return result;
            }

            if (response.finish_reason == "error") {
                if (is_context_overflow_error(response.content) &&
                    !request_history.empty()) {
                    const int removed =
                        remove_oldest_history_item(request_history);
                    result.compaction_request_items_removed += removed;
                    LOG_WARN("Context window exceeded while compacting; removed one oldest "
                             "history item and any paired tool item");
                    continue;
                }
                result.error = is_context_overflow_error(response.content)
                    ? "Context window exceeded while compacting with no removable history item."
                    : "Summarization failed: " + response.content;
                return result;
            }

            summary_suffix = response.content;
            break;
        } catch (const std::exception& error) {
            const std::string message = error.what();
            if (is_context_overflow_error(message) && !request_history.empty()) {
                const int removed = remove_oldest_history_item(request_history);
                result.compaction_request_items_removed += removed;
                LOG_WARN("Context window exceeded while compacting; removed one oldest "
                         "history item and any paired tool item");
                continue;
            }
            result.error = is_context_overflow_error(message)
                ? "Context window exceeded while compacting with no removable history item."
                : "Summarization failed: " + message;
            return result;
        }
    }

    result.compacted_messages = build_compacted_history(
        original_history, summary_suffix, COMPACT_USER_MESSAGE_MAX_TOKENS);
    const int before_tokens = estimate_message_tokens(original_history);
    const int after_tokens = estimate_message_tokens(result.compacted_messages);
    const int retained_user_messages =
        std::max(0, static_cast<int>(result.compacted_messages.size()) - 1);

    result.performed = true;
    result.messages_compressed = std::max(
        0, static_cast<int>(original_history.size()) - retained_user_messages);
    result.estimated_tokens_saved = std::max(0, before_tokens - after_tokens);
    result.summary_text = std::move(summary_suffix);

    LOG_INFO("Compact complete; trigger=" + compact_trigger_name(is_auto) +
             " history_items_before=" + std::to_string(original_history.size()) +
             " history_items_after=" +
             std::to_string(result.compacted_messages.size()) +
             " request_items_removed=" +
             std::to_string(result.compaction_request_items_removed) +
             " estimated_tokens_saved=" +
             std::to_string(result.estimated_tokens_saved));
    return result;
}

} // namespace acecode
