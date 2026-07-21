#pragma once

#include "../provider/llm_provider.hpp"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode {

constexpr std::size_t COMPACT_USER_MESSAGE_MAX_TOKENS = 20000;
constexpr int EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;
constexpr int AUTO_COMPACT_CONTEXT_WINDOW_PERCENT = 90;

struct CompactResult {
    bool performed = false;
    int messages_compressed = 0;
    int estimated_tokens_saved = 0;
    int compaction_request_items_removed = 0;
    std::string summary_text;
    std::vector<ChatMessage> compacted_messages;
    std::string error;
};

struct TokenWarningState {
    double percent_left = 100.0;
    bool is_above_warning = false;
    bool is_above_error = false;
    bool is_above_auto_compact = false;
};

// Codex uses an intentionally simple UTF-8 byte estimate: ceil(bytes / 4).
std::size_t approx_token_count(const std::string& text);

// Codex's token truncation keeps the beginning and end around a marker.
std::string truncate_text_to_token_budget(const std::string& text,
                                          std::size_t max_tokens);

int estimate_message_tokens(const std::vector<ChatMessage>& messages);

bool is_real_user_message(const ChatMessage& msg);
bool is_compact_summary_message(const ChatMessage& msg);

std::vector<ChatMessage> build_compacted_history(
    const std::vector<ChatMessage>& messages,
    const std::string& summary_text,
    std::size_t max_user_message_tokens = COMPACT_USER_MESSAGE_MAX_TOKENS);

std::vector<ChatMessage> normalize_messages_for_api(
    const std::vector<ChatMessage>& messages);

int get_effective_context_window(int context_window);
int get_auto_compact_threshold(int context_window);

bool should_auto_compact(int context_window,
                         int server_total_tokens,
                         int current_request_estimated_tokens);

TokenWarningState calculate_token_warning_state(int estimated_tokens,
                                                 int context_window);

bool is_context_overflow_error(const ProviderErrorInfo& info);
bool is_context_overflow_error(const std::string& error_message);

// Run Codex-compatible local compaction. initial_context contains stable
// base/session instructions that are always retained during overflow retries.
CompactResult compact_messages(
    LlmProvider& provider,
    const std::vector<ChatMessage>& messages,
    const std::vector<ChatMessage>& initial_context = {},
    bool is_auto = false,
    std::atomic<bool>* abort_flag = nullptr);

} // namespace acecode
