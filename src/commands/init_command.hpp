#pragma once

#include "command_registry.hpp"

#include <filesystem>
#include <string>

namespace acecode {

// Generate the ACECODE.md skeleton text for a directory. The hint block at
// the top is included only when a legacy `CLAUDE.md` or `AGENT.md` already
// lives in the same directory. Pure function — unit tested directly.
std::string build_acecode_md_skeleton(const std::filesystem::path& cwd);

// Register `/init` — generate an ACECODE.md skeleton in cwd. Refuses to
// overwrite an existing ACECODE.md. If CLAUDE.md / AGENT.md is present, the
// generated skeleton includes a migration hint comment.
void register_init_command(CommandRegistry& registry);

} // namespace acecode
