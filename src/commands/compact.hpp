#pragma once

#include "../provider/llm_provider.hpp"
#include "../tui_state.hpp"
#include "../utils/token_tracker.hpp"

#include <string>
#include <vector>
#include <optional>
#include <atomic>

namespace acecode {

// Forward declaration
class AgentLoop;

// ============================================================
// Constants
// ============================================================

constexpr int AUTOCOMPACT_BUFFER_TOKENS = 13000;
constexpr int MAX_OUTPUT_TOKENS_RESERVED = 20000;
constexpr int MAX_PTL_RETRIES = 3;
constexpr int MAX_CONSECUTIVE_AUTOCOMPACT_FAILURES = 3;
constexpr int MICRO_COMPACT_KEEP_TURNS = 3; // preserve last N assistant turns

// ============================================================
// Structs
// ============================================================

struct CompactResult {
    bool performed = false;
    int messages_compressed = 0;
    int estimated_tokens_saved = 0;
    std::string error;
};

struct AutoCompactTrackingState {
    int consecutive_failures = 0;
    bool compacted = false;
    int turn_counter = 0;
};

struct TokenWarningState {
    double percent_left = 100.0;
    bool is_above_warning = false;
    bool is_above_error = false;
    bool is_above_auto_compact = false;
};

// ============================================================
// Token estimation
// ============================================================

// Estimate total tokens from a message list (characters / 4 heuristic)
int estimate_message_tokens(const std::vector<ChatMessage>& messages);

// ============================================================
// Compact Boundary (tasks 2.1-2.4)
// ============================================================

// Create a compact boundary marker message
ChatMessage create_compact_boundary_message(const std::string& trigger, int pre_tokens);

// Check if a message is a compact boundary marker
bool is_compact_boundary_message(const ChatMessage& msg);

// Find the index of the last compact boundary in the message list (-1 if none)
int find_last_compact_boundary_index(const std::vector<ChatMessage>& messages);

// Return messages after the last compact boundary (inclusive). If no boundary, returns all.
// Returns a pair of (start_index, count) into the original vector.
std::pair<int, int> get_messages_after_compact_boundary(const std::vector<ChatMessage>& messages);

// ============================================================
// API message filtering (task 2.5)
// ============================================================

// Filter out is_meta=true messages for API calls
std::vector<ChatMessage> normalize_messages_for_api(const std::vector<ChatMessage>& messages);

// ============================================================
// Full Compact (tasks 5.x)
// ============================================================

// Perform context compaction with new boundary-aware pipeline
CompactResult compact_context(
    LlmProvider& provider,
    AgentLoop& agent_loop,
    TuiState& state,
    int keep_turns = 4,
    bool is_auto = false,
    std::atomic<bool>* abort_flag = nullptr
);

// ============================================================
// Auto-compact threshold (tasks 7.x)
// ============================================================

// Calculate effective context window size
int get_effective_context_window(int context_window);

// Calculate auto-compact threshold
int get_auto_compact_threshold(int context_window);

// Check if auto-compact should trigger.
// When last_api_prompt_tokens > 0 (from API response), uses that directly.
// Otherwise falls back to estimate_message_tokens() heuristic.
bool should_auto_compact(const std::vector<ChatMessage>& messages, int context_window, int last_api_prompt_tokens = 0);

// Calculate token warning state
TokenWarningState calculate_token_warning_state(int estimated_tokens, int context_window);

// ============================================================
// PTL retry helpers (tasks 6.x)
// ============================================================

struct ApiRoundGroup {
    int start_index = 0;
    int end_index = 0; // exclusive
};

// Group messages into API round groups (user -> assistant -> tool_results)
std::vector<ApiRoundGroup> group_messages_by_api_round(const std::vector<ChatMessage>& messages);

// Truncate oldest round groups for PTL retry
std::vector<ChatMessage> truncate_head_for_ptl_retry(
    const std::vector<ChatMessage>& messages,
    int groups_to_drop = 0
);

} // namespace acecode
