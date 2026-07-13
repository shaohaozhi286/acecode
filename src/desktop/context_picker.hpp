#pragma once

#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

// Result from the Desktop composer context picker. Normal Open returns one or
// more file paths. The custom folder action returns exactly one folder path.
struct ContextPickOutcome {
    std::vector<std::string> file_paths;
    std::optional<std::string> folder_path;
    std::string error;
};

// Opens one native window for files and folders. On Windows the common file
// dialog keeps normal file multi-selection and adds a "select current folder"
// action. Empty paths with no error mean that the user cancelled.
ContextPickOutcome pick_context_items(void* parent_hwnd,
                                      const std::string& default_folder = {});

} // namespace acecode::desktop
