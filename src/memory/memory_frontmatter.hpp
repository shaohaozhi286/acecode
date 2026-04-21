#pragma once

#include "memory_types.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace acecode {

// Parse a memory entry file (MEMORY_<name>.md or similar). Returns nullopt
// when frontmatter is unusable, a required field is missing, or `type` is
// unknown. The returned MemoryEntry.path is set to `path`, name is derived
// from filename (not frontmatter) so on-disk and in-memory names stay in sync.
std::optional<MemoryEntry> parse_memory_entry_file(const std::filesystem::path& path);

// Render a memory entry back to disk format: frontmatter + two-line separator
// + body. Used by memory_write for atomic rewrite.
std::string render_memory_entry(const MemoryEntry& entry);

} // namespace acecode
