// /model slash command —— display the picker or switch the active model entry.
// Sub-actions:
//   /model                       — list saved_models + (legacy), mark current with '*'
//   /model <name>                — in-memory switch
//   /model --cwd <name>          — switch + persist to <cwd_hash>/model_override.json
//   /model --default <name>      — switch + persist to config.json default_model_name
#pragma once

#include "command_registry.hpp"

namespace acecode {

void register_model_command(CommandRegistry& registry);

} // namespace acecode
