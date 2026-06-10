#pragma once

#include "../config/config.hpp"
#include "../session/session_client.hpp"
#include "../session/session_storage.hpp"
#include "../tui_state.hpp"
#include "../utils/token_tracker.hpp"

#include <optional>

namespace acecode {

inline bool saved_model_name_exists(const AppConfig& config, const std::string& name) {
    if (name.empty()) return false;
    for (const auto& entry : config.saved_models) {
        if (entry.name == name) return true;
    }
    return false;
}

inline std::optional<SessionModelState> deleted_model_state_from_meta(
    const AppConfig& config,
    const SessionMeta& meta) {
    if (meta.model_preset.empty()) return std::nullopt;
    if (saved_model_name_exists(config, meta.model_preset)) return std::nullopt;
    SessionModelState state;
    state.name = meta.model_preset;
    state.deleted = true;
    return state;
}

inline std::string tui_model_status_line(const SessionModelState& model_state) {
    if (model_state.deleted && !model_state.name.empty()) {
        return model_state.name + " (deleted)";
    }
    if (!model_state.provider.empty() || !model_state.model.empty()) {
        return "[" + model_state.provider + "] model: " + model_state.model;
    }
    return {};
}

// Caller owns any required TuiState locking. The /resume picker invokes this
// while state.mu is already held by the event handler.
inline void sync_tui_resume_runtime_state(
    TuiState& state,
    const AppConfig& config,
    TokenTracker& token_tracker,
    const std::optional<SessionModelState>& model_state) {
    if (model_state.has_value()) {
        std::string status = tui_model_status_line(*model_state);
        if (!status.empty()) state.status_line = std::move(status);
    }
    state.token_status = token_tracker.format_status(config.context_window);
    state.token_percent = token_tracker.context_percent(config.context_window);
}

} // namespace acecode
