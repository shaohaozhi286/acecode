#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace acecode::desktop {

struct OpenInExplorerResult {
    bool ok = false;
    std::string error;
};

struct ValidatedOpenDirectory {
    bool ok = false;
    std::filesystem::path path;
    std::string error;
};

using DirectoryOpenLauncher = std::function<bool(const std::filesystem::path&, std::string&)>;

ValidatedOpenDirectory validate_open_directory_request(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8 = {});

OpenInExplorerResult open_directory_in_file_manager(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8 = {},
    DirectoryOpenLauncher launcher = {});

} // namespace acecode::desktop
