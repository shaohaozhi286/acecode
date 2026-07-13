#include "path_reference_input.hpp"

#include "../tui_state.hpp"
#include "picker_scroll.hpp"

#include <algorithm>

namespace acecode::tui {

namespace {

std::string token_signature(const TuiState& state,
                            const path_reference::Token& token) {
    return state.input_text.substr(token.begin, token.end - token.begin) + "@" +
        std::to_string(token.begin) + ":" + std::to_string(token.end);
}

void clear_visible_state(TuiState& state) {
    state.path_reference_active = false;
    state.path_reference_token.reset();
    state.path_reference_items.clear();
    state.path_reference_selected = 0;
    state.path_reference_view_offset = 0;
    state.path_reference_error.clear();
}

} // namespace

void clear_path_reference_state(TuiState& state, bool clear_dismissal) {
    clear_visible_state(state);
    if (clear_dismissal) state.path_reference_dismissed_token.clear();
    ++state.path_reference_generation;
}

void refresh_path_reference_state(TuiState& state, const std::string& cwd) {
    if (state.input_mode != InputMode::Normal || state.resume_picker_active ||
        state.rewind_picker_active || state.model_picker_open ||
        state.confirm_pending || state.ask_pending) {
        clear_visible_state(state);
        return;
    }

    auto token = path_reference::token_at_cursor(state.input_text,
                                                  state.input_cursor);
    if (!token.has_value()) {
        clear_path_reference_state(state, true);
        return;
    }
    const std::string signature = token_signature(state, *token);
    if (!state.path_reference_dismissed_token.empty() &&
        state.path_reference_dismissed_token == signature) {
        clear_visible_state(state);
        return;
    }
    if (state.path_reference_dismissed_token != signature) {
        state.path_reference_dismissed_token.clear();
    }

    std::string previous_path;
    if (state.path_reference_active && state.path_reference_selected >= 0 &&
        state.path_reference_selected <
            static_cast<int>(state.path_reference_items.size())) {
        previous_path = state.path_reference_items[state.path_reference_selected].path;
    }

    auto result = path_reference::suggest(cwd, token->path);
    state.path_reference_token = *token;
    state.path_reference_items = std::move(result.items);
    state.path_reference_error = std::move(result.error);
    state.path_reference_active = true;
    ++state.path_reference_generation;

    int selected = 0;
    if (!previous_path.empty()) {
        for (int i = 0; i < static_cast<int>(state.path_reference_items.size()); ++i) {
            if (state.path_reference_items[i].path == previous_path) {
                selected = i;
                break;
            }
        }
    }
    state.path_reference_selected = selected;
    state.path_reference_view_offset = scroll_to_keep_visible(
        selected, state.path_reference_view_offset, kPathReferenceVisibleRows,
        static_cast<int>(state.path_reference_items.size()));
}

void move_path_reference_selection(TuiState& state, int delta) {
    const int count = static_cast<int>(state.path_reference_items.size());
    if (count <= 0) return;
    state.path_reference_selected =
        (state.path_reference_selected + delta % count + count) % count;
    state.path_reference_view_offset = scroll_to_keep_visible(
        state.path_reference_selected, state.path_reference_view_offset,
        kPathReferenceVisibleRows, count);
}

bool commit_path_reference_selection(TuiState& state, bool enter_directory) {
    if (!state.path_reference_active || !state.path_reference_token.has_value() ||
        state.path_reference_selected < 0 || state.path_reference_selected >=
            static_cast<int>(state.path_reference_items.size())) {
        return false;
    }
    const auto item = state.path_reference_items[state.path_reference_selected];
    if (enter_directory && !item.is_directory) return false;
    auto replacement = path_reference::replace_token(
        state.input_text, *state.path_reference_token, item.path,
        item.is_directory, enter_directory);
    state.input_text = std::move(replacement.text);
    state.input_cursor = replacement.cursor;
    clear_path_reference_state(state, true);
    return true;
}

void dismiss_path_reference_state(TuiState& state) {
    if (state.path_reference_token.has_value()) {
        state.path_reference_dismissed_token =
            token_signature(state, *state.path_reference_token);
    }
    clear_visible_state(state);
}

} // namespace acecode::tui
