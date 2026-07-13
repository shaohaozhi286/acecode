#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <vector>

namespace acecode {
struct TuiState;
}

namespace acecode::tui {

ftxui::Element render_path_reference_dropdown(
    const TuiState& state,
    bool conhost_compat_layout,
    std::vector<ftxui::Box>& row_boxes);

} // namespace acecode::tui
