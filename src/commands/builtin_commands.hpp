#pragma once

#include "command_registry.hpp"

namespace acecode {

// Register all builtin slash commands (/help, /clear, /new, /model, /config, /tokens, /compact, /exit)
void register_builtin_commands(CommandRegistry& registry);

// Resume an exact persisted session without going through the numeric picker.
// Used by Windows notification activation; runs the same canonical restoration
// path as `/resume <number>` and takes the TUI state lock internally.
bool resume_session_by_id(CommandContext& ctx, const std::string& session_id);

} // namespace acecode
