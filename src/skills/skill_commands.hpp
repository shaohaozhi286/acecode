#pragma once

#include <vector>
#include <string>

namespace acecode {

class CommandRegistry;
class SkillRegistry;

// Register one slash command per skill currently in the registry. Commands
// whose key collides with a built-in are skipped with a logger warning. The
// returned vector lists the command keys actually registered (caller uses this
// to unregister them on /skills reload).
std::vector<std::string> register_skill_commands(CommandRegistry& cmd_registry,
                                                 SkillRegistry& skill_registry);

// Remove previously-registered skill commands from cmd_registry.
void unregister_skill_commands(CommandRegistry& cmd_registry,
                               const std::vector<std::string>& command_keys);

// Convenience: unregister the previously-registered skill commands (tracked
// internally), re-scan the SkillRegistry, and re-register fresh commands.
// Returns the count of commands registered after the reload. Intended to back
// the `/skills reload` built-in.
size_t reload_skill_commands(CommandRegistry& cmd_registry,
                             SkillRegistry& skill_registry);

// Like register_skill_commands, but also remembers the registered keys in a
// module-level store so reload_skill_commands can clean them up. Use this one
// from startup; use register_skill_commands when the caller tracks its own
// keys.
std::vector<std::string> register_skill_commands_tracked(CommandRegistry& cmd_registry,
                                                         SkillRegistry& skill_registry);

} // namespace acecode
