#pragma once
// Agent callbacks construction extracted from main.cpp.
// Converts lambda-based callbacks to named free functions.
#include "tui/tui_context.hpp"

namespace acecode { namespace tui {

// Build and install all agent callbacks on ctx.agent_loop.
// Requires ctx to be fully populated.
void setup_agent_callbacks(TuiContext& ctx);

}} // namespace acecode::tui
