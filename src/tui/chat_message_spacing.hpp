#pragma once

#include <cstddef>
#include <vector>

#include "tui_state.hpp"

namespace acecode::tui {

// Preserve the original spacer after every message, except between adjacent
// system notices and between an adjacent tool call/result pair.
inline std::vector<int> chat_message_spacer_rows_after(
    const std::vector<TuiState::Message>& messages) {
    std::vector<int> rows(messages.size(), 1);
    for (std::size_t i = 0; i + 1 < messages.size(); ++i) {
        const bool compact_system_notices =
            messages[i].role == "system" &&
            messages[i + 1].role == "system";
        const bool compact_tool_result =
            messages[i].role == "tool_call" &&
            messages[i + 1].role == "tool_result";
        if (compact_system_notices || compact_tool_result) {
            rows[i] = 0;
        }
    }
    return rows;
}

} // namespace acecode::tui
