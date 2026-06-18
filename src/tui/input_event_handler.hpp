#pragma once
// Input event handler extracted from main.cpp CatchEvent lambda.
// ~2077 lines of keyboard/mouse event handling.
#include <ftxui/component/event.hpp>
#include "tui/tui_context.hpp"

namespace acecode { namespace tui {

// Handle all keyboard/mouse input events.
// Returns true if the event was consumed, false to pass through.
bool handle_input_event(TuiContext& ctx, ftxui::Event event);

}} // namespace acecode::tui
