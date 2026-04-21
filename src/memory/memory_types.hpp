#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace acecode {

// Four memory categories mirroring Claude Code semantics. Saved verbatim as the
// frontmatter `type` field of each entry file.
enum class MemoryType {
    User = 0,      // persistent facts about the user (role, preferences)
    Feedback = 1,  // collaboration style / dos & donts the user has given us
    Project = 2,   // ongoing work context not derivable from the codebase
    Reference = 3, // pointers to external systems (dashboards, Linear projects)
};

// Convert between MemoryType and its wire/frontmatter string form.
// parse_memory_type returns nullopt for unknown values; callers must decide
// whether to skip the entry or log a warning.
std::string memory_type_to_string(MemoryType t);
std::optional<MemoryType> parse_memory_type(const std::string& s);

// One memory entry loaded from a file under ~/.acecode/memory/<name>.md.
// `body` is the post-frontmatter markdown; `path` is the absolute on-disk path
// so tools/commands can report it to the user.
struct MemoryEntry {
    std::string name;
    std::string description;
    MemoryType type = MemoryType::User;
    std::filesystem::path path;
    std::string body;
};

} // namespace acecode
