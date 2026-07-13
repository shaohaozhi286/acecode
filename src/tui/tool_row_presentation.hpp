#pragma once

#include "tui/theme_palette.hpp"

namespace acecode::tui {

inline bool tool_call_arguments_visible(bool transcript_expanded) {
    return transcript_expanded;
}

inline Color tool_call_name_color(const ThemePalette& palette) {
    return palette.name == "dark"
        ? palette.ui.text_primary
        : palette.syntax.preproc;
}

inline Color tool_call_argument_color(const ThemePalette& palette) {
    return palette.ui.text_muted;
}

inline Color tool_result_text_color(const ThemePalette& palette) {
    return palette.ui.text_muted;
}

} // namespace acecode::tui
