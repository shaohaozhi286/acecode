#pragma once

#include "../tui_state.hpp"

#include <ftxui/dom/elements.hpp>

namespace acecode {

// Render the live tool-progress element shown between message_view and
// prompt_line while state.tool_running is true. Returns text("") when not
// running. Caller holds state.mu.
ftxui::Element render_tool_progress(const TuiState& state);

// Render the compact timer chip shown in the bottom status bar while
// state.tool_running is true. Returns text("") when not running.
ftxui::Element render_tool_timer_chip(const TuiState& state);

// Render the compact waiting-indicator chip shown in the bottom status bar
// while the agent is waiting on the LLM (state.is_waiting && !tool_running).
// Shows the current thinking phrase and elapsed seconds; after
// SHOW_TOKENS_AFTER_MS it also shows either the authoritative completion
// token count (from on_usage) or a live `~N tok` estimate derived from
// state.streaming_output_chars. Returns text("") when the chip is hidden.
ftxui::Element render_thinking_timer_chip(const TuiState& state);

} // namespace acecode
