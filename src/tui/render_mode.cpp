#include "render_mode_factory.hpp"

namespace acecode::tui {

ftxui::ScreenInteractive make_screen_interactive(ScreenRenderMode mode) {
    switch (mode) {
        case ScreenRenderMode::AltScreen:
            return ftxui::ScreenInteractive::Fullscreen();
        case ScreenRenderMode::TerminalOutput:
        default:
            return ftxui::ScreenInteractive::TerminalOutput();
    }
}

} // namespace acecode::tui
