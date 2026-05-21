#pragma once

#include "command_registry.hpp"

namespace acecode {

// Register all builtin slash commands (/help, /clear, /new, /model, /config, /tokens, /compact, /exit)
void register_builtin_commands(CommandRegistry& registry);

} // namespace acecode
