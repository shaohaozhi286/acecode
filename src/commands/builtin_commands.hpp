#pragma once

#include "command_registry.hpp"

namespace acecode {

// Register all builtin slash commands (/help, /clear, /model, /config, /tokens, /compact, /exit)
void register_builtin_commands(CommandRegistry& registry);

} // namespace acecode
