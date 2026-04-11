#pragma once

#include <string>

namespace acecode {

// Generate a unified diff between old and new content for a given file path.
std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path
);

} // namespace acecode
