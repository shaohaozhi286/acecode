#include "input_history_navigation.hpp"

#include <utility>

namespace acecode::tui {

namespace {

constexpr int kClearedInputRestoreIndex = -2;

} // namespace

bool try_cancel_latest_pending_for_history_text(TuiState& state,
                                                const std::string& text) {
    if (state.pending_queue.empty() || state.pending_queue.back() != text) {
        return false;
    }
    state.pending_queue.pop_back();
    if (!state.pending_structured_queue.empty() &&
        state.pending_structured_queue.back().display_text == text) {
        state.pending_structured_queue.pop_back();
    }
    return true;
}

bool clear_current_input_for_history_restore(TuiState& state) {
    if (state.input_text.empty() && state.input_mode == InputMode::Normal) {
        return false;
    }

    state.saved_input = prepend_mode_prefix(state.input_text, state.input_mode);
    state.history_index = kClearedInputRestoreIndex;
    state.input_mode = InputMode::Normal;
    state.input_text.clear();
    state.input_cursor = 0;
    return true;
}

bool navigate_input_history_up(TuiState& state) {
    if (state.history_index == kClearedInputRestoreIndex) {
        auto [saved_mode, saved_text] = parse_mode_prefix(state.saved_input);
        state.input_mode = saved_mode;
        state.input_text = std::move(saved_text);
        state.input_cursor = state.input_text.size();
        state.history_index = -1;
        state.saved_input.clear();
        return true;
    }

    if (state.input_history.empty()) {
        return false;
    }

    if (state.history_index == -1) {
        state.saved_input = prepend_mode_prefix(state.input_text, state.input_mode);
        state.history_index = static_cast<int>(state.input_history.size()) - 1;
    } else if (state.history_index > 0) {
        --state.history_index;
    }

    if (state.history_index < 0 ||
        state.history_index >= static_cast<int>(state.input_history.size())) {
        state.history_index = -1;
        return false;
    }

    auto [hist_mode, hist_text] =
        parse_mode_prefix(state.input_history[state.history_index]);
    if (hist_mode == InputMode::Normal) {
        try_cancel_latest_pending_for_history_text(state, hist_text);
    }

    state.input_mode = hist_mode;
    state.input_text = std::move(hist_text);
    state.input_cursor = state.input_text.size();
    return true;
}

bool navigate_input_history_down(TuiState& state) {
    if (state.history_index == -1 ||
        state.history_index == kClearedInputRestoreIndex) {
        return false;
    }

    if (state.input_history.empty() ||
        state.history_index >= static_cast<int>(state.input_history.size())) {
        state.history_index = -1;
        auto [saved_mode, saved_text] = parse_mode_prefix(state.saved_input);
        state.input_mode = saved_mode;
        state.input_text = std::move(saved_text);
        state.input_cursor = state.input_text.size();
        return true;
    }

    if (state.history_index < static_cast<int>(state.input_history.size()) - 1) {
        ++state.history_index;
        auto [hist_mode, hist_text] =
            parse_mode_prefix(state.input_history[state.history_index]);
        state.input_mode = hist_mode;
        state.input_text = std::move(hist_text);
    } else {
        state.history_index = -1;
        auto [saved_mode, saved_text] = parse_mode_prefix(state.saved_input);
        state.input_mode = saved_mode;
        state.input_text = std::move(saved_text);
    }

    state.input_cursor = state.input_text.size();
    return true;
}

} // namespace acecode::tui
