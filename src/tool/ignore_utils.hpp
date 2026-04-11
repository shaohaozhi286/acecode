#pragma once

#include <string>
#include <set>

namespace acecode {

// Default directories to skip during recursive traversal
inline const std::set<std::string>& ignored_directories() {
    static const std::set<std::string> dirs = {
        ".git", "node_modules", "build", "build2",
        "__pycache__", ".cache", ".vs", ".vscode",
        "dist", "out", ".next", "target",
        "Debug", "Release", "x64", "x86",
    };
    return dirs;
}

// Check if a directory name should be skipped
inline bool should_ignore_dir(const std::string& dir_name) {
    return ignored_directories().count(dir_name) > 0;
}

} // namespace acecode
