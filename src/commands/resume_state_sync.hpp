#pragma once

#include "../config/config.hpp"
#include "../session/session_client.hpp"
#include "../tui_state.hpp"
#include "../utils/token_tracker.hpp"

#include <optional>

namespace acecode {

// Caller owns any required TuiState locking. The /resume picker invokes this
// while state.mu is already held by the event handler.
inline void sync_tui_resume_runtime_state(
    TuiState& state,
    const AppConfig& config,
    TokenTracker& token_tracker,
    const std::optional<SessionModelState>& model_state) {
    if (model_state.has_value() &&
        (!model_state->provider.empty() || !model_state->model.empty())) {
        state.status_line = "[" + model_state->provider + "] model: " +
                            model_state->model;
    }
    state.token_status = token_tracker.format_status(config.context_window);
    state.token_percent = token_tracker.context_percent(config.context_window);
}

} // namespace acecode
