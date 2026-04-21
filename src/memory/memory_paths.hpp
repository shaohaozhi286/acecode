#pragma once

#include <filesystem>
#include <string>

namespace acecode {

// Absolute path to ~/.acecode/memory/ (computed from get_acecode_dir()).
// Does NOT create the directory.
std::filesystem::path get_memory_dir();

// Absolute path to ~/.acecode/memory/MEMORY.md (index file).
std::filesystem::path get_memory_index_path();

// Reject names that would escape the memory directory or include separators.
// Returns empty string on success; otherwise a rejection reason.
std::string validate_memory_name(const std::string& name);

// Resolve <name>.md inside the memory directory. Returns the absolute path
// on success. On failure (bad name or path escapes memory dir after symlink
// resolution) returns empty path; callers should treat as validation error.
std::filesystem::path resolve_memory_entry_path(const std::string& name);

// True iff `path` resolves to a file strictly inside get_memory_dir().
// Used by memory_write as a final guard after name sanitization.
bool is_within_memory_dir(const std::filesystem::path& path);

} // namespace acecode
