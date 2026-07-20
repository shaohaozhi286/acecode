#pragma once

#include "../provider/llm_provider.hpp"

#include <string>
#include <vector>
#include <set>

namespace acecode {

// Set of tool names whose results can be cleared by micro-compact
extern const std::set<std::string> COMPACTABLE_TOOLS;

struct MicroCompactResult {
    bool performed = false;
    int tool_results_cleared = 0;
    int estimated_tokens_saved = 0;
    std::vector<std::string> cleared_tool_call_ids;
};

// Run micro-compact: clear old tool results in-place.
// Only clears tool results that are:
// 1. From tools in the COMPACTABLE_TOOLS set (read/search/shell only —
//    never file_edit/file_write; wiping mutations causes rewrite loops)
// 2. Older than the last `keep_user_turns` countable user turns
//    (default matches full-compact keep_turns so auto-compact does not
//    erase the window full-compact is about to retain)
// If fewer than keep_user_turns exist, clears nothing (protect everything).
// Returns info about what was cleared.
MicroCompactResult run_micro_compact(
    std::vector<ChatMessage>& messages,
    int boundary_start,
    int keep_user_turns = 4
);

// Create a microcompact boundary marker message
ChatMessage create_microcompact_boundary_message(
    int pre_tokens,
    int tokens_saved,
    const std::vector<std::string>& cleared_ids
);

} // namespace acecode
