#pragma once

#include "provider/llm_provider.hpp"
#include "tui_state.hpp"

#include <vector>

namespace acecode { namespace tui {

inline constexpr const char* kCollapsedCompactNoticeLabel =
    "Context compacted";

// Append one tagged compact transcript message to the TUI projection. Messages
// from the same operation share one runtime row: it stays expanded while the
// operation is incomplete, then collapses when the successful completion edge
// arrives. Returns false for ordinary transcript messages.
bool append_compact_notice_row(std::vector<TuiState::Message>& rows,
                               const ChatMessage& message);

bool compact_notice_row_is_expanded(const TuiState::Message& row,
                                    bool transcript_expanded);

// Toggle a completed compact row for the contextual Ctrl+E action. Incomplete
// rows cannot be hidden and ordinary rows are outside this interaction.
bool toggle_completed_compact_notice_row(TuiState::Message& row);

}} // namespace acecode::tui
