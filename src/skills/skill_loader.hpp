#pragma once

#include "skill_metadata.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// Load frontmatter-only metadata for a skill directory. Returns nullopt when
// SKILL.md is missing, unreadable, or fails all parse attempts.
std::optional<SkillMetadata> load_skill_from_dir(const std::filesystem::path& dir,
                                                 const std::filesystem::path& scan_root);

// Return the current runtime OS identifier used for platform filtering
// ("windows", "macos", "linux", or "unknown").
std::string current_platform_identifier();

// Return true when the skill is compatible with the current platform. Empty
// platforms list is treated as "all platforms".
bool skill_matches_platform(const std::vector<std::string>& platforms);

// Convert a skill name to its kebab-case slash-command key. Lowercases,
// replaces spaces/underscores with '-', strips characters outside [a-z0-9-],
// and collapses runs of hyphens. Returns empty string when nothing usable
// remains.
std::string normalize_skill_command_key(const std::string& name);

} // namespace acecode
