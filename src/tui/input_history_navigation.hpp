#pragma once

#include "../tui_state.hpp"

#include <string>

namespace acecode::tui {

bool try_cancel_latest_pending_for_history_text(TuiState& state,
                                                const std::string& text);

bool clear_current_input_for_history_restore(TuiState& state);

bool navigate_input_history_up(TuiState& state);

bool navigate_input_history_down(TuiState& state);

} // namespace acecode::tui
