#pragma once

#include "command_registry.hpp"

namespace acecode {

// Register the /models slash command with the supplied registry.
// Subcommands: info, refresh [--network], lookup <model-id>.
void register_models_command(CommandRegistry& registry);

} // namespace acecode
