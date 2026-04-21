#pragma once

#include "memory_types.hpp"

#include <string>
#include <vector>

namespace acecode {

// Bidirectional helpers for the MEMORY.md index file. The index is a simple
// markdown document where each managed line has the shape:
//   `- [<description>](<stem>.md) — <description>`
// Everything else (preamble, comments, unrelated lines) is preserved verbatim
// by render_memory_index so a user's hand-authored notes in MEMORY.md are not
// clobbered by memory_write.

// Render a complete MEMORY.md from current entries. `existing_text` is the
// current on-disk MEMORY.md (empty string when the file doesn't exist). Lines
// in `existing_text` that reference a known entry are updated in place; lines
// referencing unknown entries are dropped (their entry file is gone); entries
// not present anywhere in the file are appended to the end.
std::string render_memory_index(const std::vector<MemoryEntry>& entries,
                                const std::string& existing_text);

// Return the single-line index form of one entry.
std::string format_memory_index_line(const MemoryEntry& entry);

// Strip an entry's index line from `existing_text`. If no matching line
// exists, returns `existing_text` unchanged. Used by memory_registry.remove().
std::string remove_memory_index_line(const std::string& existing_text,
                                     const std::string& name);

} // namespace acecode
