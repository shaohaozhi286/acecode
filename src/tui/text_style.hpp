#pragma once

#include "theme_palette.hpp"

#include <ftxui/dom/elements.hpp>

namespace acecode::tui {

// Low-emphasis copy that still needs to be read: placeholders, passive states,
// status text, and interaction hints. Keep terminal `dim` out of this style so
// dark themes do not attenuate an already-muted color a second time.
inline ftxui::Decorator readable_secondary() {
    return ftxui::color(theme().ui.text_secondary);
}

} // namespace acecode::tui
