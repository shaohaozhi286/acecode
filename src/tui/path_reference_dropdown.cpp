#include "path_reference_dropdown.hpp"

#include "../tui_state.hpp"
#include "path_reference_input.hpp"
#include "theme_palette.hpp"

#include <algorithm>
#include <string>

namespace acecode::tui {

ftxui::Element render_path_reference_dropdown(
    const TuiState& state,
    bool conhost_compat_layout,
    std::vector<ftxui::Box>& row_boxes) {
    using namespace ftxui;
    row_boxes.assign(state.path_reference_items.size(), Box{});
    if (!state.path_reference_active) return emptyElement();

    Elements rows;
    if (!state.path_reference_error.empty()) {
        rows.push_back(text("  " + state.path_reference_error) |
                       color(theme().ui.text_dim));
    }
    if (state.path_reference_items.empty()) {
        if (state.path_reference_error.empty()) {
            rows.push_back(text("  No matching files or folders") |
                           color(theme().ui.text_dim));
        }
    } else {
        const int total = static_cast<int>(state.path_reference_items.size());
        const int visible = std::min(kPathReferenceVisibleRows, total);
        const int offset = std::clamp(state.path_reference_view_offset, 0,
                                      std::max(0, total - visible));
        if (offset > 0) {
            rows.push_back(text("  ^ " + std::to_string(offset) + " more above") |
                           color(theme().ui.text_dim));
        }
        for (int i = offset; i < offset + visible; ++i) {
            const auto& item = state.path_reference_items[i];
            std::string label = item.is_directory ? "[D] " : "[F] ";
            label += item.path;
            if (item.is_directory) label += "/";
            if (item.is_directory && !conhost_compat_layout) {
                label += "  (Enter: reference, Right/Tab: open)";
            }
            Element row = text("  " + label);
            if (i == state.path_reference_selected) {
                row = row | bold | color(theme().ui.selection_fg) |
                    bgcolor(theme().ui.selection_bg);
            } else {
                row = row | color(theme().ui.text_muted);
            }
            rows.push_back(row | reflect(row_boxes[static_cast<std::size_t>(i)]));
        }
        const int below = total - offset - visible;
        if (below > 0) {
            rows.push_back(text("  v " + std::to_string(below) + " more below") |
                           color(theme().ui.text_dim));
        }
    }
    return vbox(std::move(rows)) | border | color(theme().ui.border);
}

} // namespace acecode::tui
