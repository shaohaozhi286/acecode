#pragma once

#include "../tui_state.hpp"

#include <ftxui/dom/elements.hpp>

namespace acecode {

class CommandRegistry;

// Re-evaluate the dropdown from the current input_text. Caller must already
// hold state.mu. Reads the command list from `reg` and updates
// slash_dropdown_* fields on `state`.
//
// Triggers the dropdown when all are true:
//   - input_text begins with '/'
//   - input_text contains no whitespace (still in command-name position)
//   - slash_dropdown_dismissed_for_input is false
//   - no other overlay is active (resume picker, tool confirmation)
//
// Clears dismissed_for_input whenever the input leaves command-name position,
// so a fresh '/' after backspacing to empty re-opens the dropdown normally.
void refresh_slash_dropdown(TuiState& state, const CommandRegistry& reg);

// Render the dropdown as a single FTXUI Element. Returns an empty element
// when not active. Caller holds state.mu.
ftxui::Element render_slash_dropdown(const TuiState& state);

} // namespace acecode
