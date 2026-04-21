#pragma once

#include "../config/config.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace acecode {

// Result of loading project-instruction files from disk.
struct MergedInstructions {
    // Paths that contributed content, in the order they appear in merged_body.
    std::vector<std::filesystem::path> sources;
    // The concatenated body, with per-file separators and a trailing newline.
    // Empty when no file was found.
    std::string merged_body;
    // True if any cap (per-file max_bytes / aggregate max_total_bytes) was hit.
    bool truncated = false;
};

// Framing sentence injected in front of the merged body inside the system
// prompt. Kept as a named constant so system_prompt.cpp and tests agree.
extern const char* kProjectInstructionsFraming;

// Pure function: given a cwd and the ProjectInstructionsConfig, walk the
// HOME→cwd chain plus `~/.acecode/`, selecting at most one file per directory
// based on the effective filenames priority (filenames ∩ per-name gates), and
// return the merged text. No caching — callers invoke this every turn so
// mid-session edits are visible.
MergedInstructions load_project_instructions(const std::string& cwd,
                                             const ProjectInstructionsConfig& cfg);

} // namespace acecode
