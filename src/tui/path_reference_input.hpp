#pragma once

#include <string>

namespace acecode {
struct TuiState;
}

namespace acecode::tui {

constexpr int kPathReferenceVisibleRows = 9;

void clear_path_reference_state(TuiState& state, bool clear_dismissal = true);
void refresh_path_reference_state(TuiState& state, const std::string& cwd);
void move_path_reference_selection(TuiState& state, int delta);
bool commit_path_reference_selection(TuiState& state, bool enter_directory);
void dismiss_path_reference_state(TuiState& state);

} // namespace acecode::tui
