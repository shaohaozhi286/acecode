#include "micro_compact.hpp"
#include "compact.hpp"
#include "../utils/logger.hpp"
#include "../utils/uuid.hpp"

#include <algorithm>

namespace acecode {

// Read / search / shell outputs are bulk noise and safe to drop once stale.
// Never include file_edit / file_write: clearing mutation confirmations makes
// the model re-apply the same edits in a loop after compact.
const std::set<std::string> COMPACTABLE_TOOLS = {
    "file_read",
    "bash", "shell",
    "grep", "glob",
    "web_fetch"
};

// Quiet placeholder: avoid words like "compressed" / "cleared" that make
// models believe the whole conversation was wiped and re-read everything.
const char* kMicroCompactOmittedPlaceholder =
    "[Older tool output omitted from context]";

namespace {

// Find the tool name for a given tool_call_id by scanning assistant messages
std::string find_tool_name_for_call_id(
    const std::vector<ChatMessage>& messages,
    int start, int end,
    const std::string& tool_call_id
) {
    for (int i = start; i < end; ++i) {
        const auto& msg = messages[i];
        if (msg.role == "assistant" && !msg.tool_calls.is_null() && msg.tool_calls.is_array()) {
            for (const auto& tc : msg.tool_calls) {
                if (tc.contains("id") && tc["id"].is_string() &&
                    tc["id"].get<std::string>() == tool_call_id) {
                    if (tc.contains("function") && tc["function"].contains("name")) {
                        return tc["function"]["name"].get<std::string>();
                    }
                }
            }
        }
    }
    return "";
}

bool is_already_omitted_tool_result(const std::string& content) {
    return content == kMicroCompactOmittedPlaceholder ||
           content == "[Old tool result content cleared]";
}

} // namespace

MicroCompactResult run_micro_compact(
    std::vector<ChatMessage>& messages,
    int boundary_start,
    int keep_user_turns
) {
    MicroCompactResult result;
    int n = static_cast<int>(messages.size());
    if (boundary_start < 0) boundary_start = 0;
    if (boundary_start >= n) return result;

    const int keep = std::max(1, keep_user_turns);

    // Protect everything from the start of the last `keep` countable user
    // turns through the end. If fewer than `keep` real user turns exist,
    // protect the entire active window (clear nothing).
    //
    // Previous logic counted assistant turns and defaulted cutoff to `n`
    // when the keep count was not reached — which wiped *all* tool results,
    // including the most recent ones. That caused post-compact amnesia loops.
    int cutoff_index = boundary_start;
    int user_count = 0;
    for (int i = n - 1; i >= boundary_start; --i) {
        if (!is_countable_user_turn(messages[i])) continue;
        ++user_count;
        if (user_count >= keep) {
            cutoff_index = i;
            break;
        }
    }

    const std::string placeholder = kMicroCompactOmittedPlaceholder;

    for (int i = boundary_start; i < cutoff_index; ++i) {
        auto& msg = messages[i];
        if (msg.role != "tool") continue;
        if (is_already_omitted_tool_result(msg.content)) continue;
        if (msg.content.empty()) continue;

        // Tool name may live on an assistant message before this tool row;
        // search the full active window, not only the droppable prefix.
        std::string tool_name = find_tool_name_for_call_id(
            messages, boundary_start, n, msg.tool_call_id);
        if (tool_name.empty() ||
            COMPACTABLE_TOOLS.find(tool_name) == COMPACTABLE_TOOLS.end()) {
            continue;
        }

        int original_chars = static_cast<int>(msg.content.size());
        int savings = (original_chars - static_cast<int>(placeholder.size())) / 4;
        if (savings <= 0) continue;

        result.estimated_tokens_saved += savings;
        result.tool_results_cleared++;
        result.cleared_tool_call_ids.push_back(msg.tool_call_id);

        msg.content = placeholder;
    }

    result.performed = result.tool_results_cleared > 0;

    if (result.performed) {
        LOG_INFO("Micro-compact: omitted " + std::to_string(result.tool_results_cleared) +
                 " old tool results, estimated ~" +
                 std::to_string(result.estimated_tokens_saved) + " tokens saved" +
                 " cutoff_index=" + std::to_string(cutoff_index) +
                 " keep_user_turns=" + std::to_string(keep));
    }

    return result;
}

ChatMessage create_microcompact_boundary_message(
    int pre_tokens,
    int tokens_saved,
    const std::vector<std::string>& cleared_ids
) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[Micro-compact boundary]";
    msg.uuid = generate_uuid();
    msg.subtype = "microcompact_boundary";
    msg.timestamp = iso_timestamp();
    msg.is_meta = true;

    nlohmann::json ids_array = nlohmann::json::array();
    for (const auto& id : cleared_ids) {
        ids_array.push_back(id);
    }

    msg.metadata = {
        {"trigger", "micro"},
        {"pre_tokens", pre_tokens},
        {"tokens_saved", tokens_saved},
        {"cleared_tool_call_ids", ids_array}
    };
    return msg;
}

} // namespace acecode
