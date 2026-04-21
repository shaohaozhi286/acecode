#pragma once

#include "command_registry.hpp"

namespace acecode {

// Register the `/memory` slash command with subcommands:
//   list [--type=<t>]  — show index, optionally filtered
//   view <name>        — print entry body
//   edit <name>        — open in $EDITOR then reload
//   forget <name>      — delete entry file + MEMORY.md line
//   reload             — rescan ~/.acecode/memory/
void register_memory_command(CommandRegistry& registry);

} // namespace acecode
