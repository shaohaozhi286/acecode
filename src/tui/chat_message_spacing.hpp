#pragma once

#include <cstddef>
#include <vector>

#include "tui_state.hpp"

namespace acecode::tui {

// Preserve the original spacer after every message, except between adjacent
// system notices. Consecutive "i" rows are one compact status group.
inline std::vector<int> chat_message_spacer_rows_after(
    const std::vector<TuiState::Message>& messages) {
    std::vector<int> rows(messages.size(), 1);
    for (std::size_t i = 0; i + 1 < messages.size(); ++i) {
        if (messages[i].role == "system" &&
            messages[i + 1].role == "system") {
            rows[i] = 0;
        }
    }
    return rows;
}

} // namespace acecode::tui
