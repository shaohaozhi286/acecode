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

enum class OpenInExplorerTargetKind {
    Directory,
    File,
};

struct ValidatedOpenTarget {
    bool ok = false;
    std::filesystem::path path;
    OpenInExplorerTargetKind kind = OpenInExplorerTargetKind::Directory;
    std::string error;
};

using DirectoryOpenLauncher = std::function<bool(const std::filesystem::path&, std::string&)>;
using OpenInExplorerLauncher = std::function<bool(
    const std::filesystem::path&,
    OpenInExplorerTargetKind,
    std::string&)>;

std::vector<std::string> append_allowed_open_root(
    std::vector<std::string> allowed_roots_utf8,
    const std::string& extra_root_utf8);

ValidatedOpenTarget validate_open_in_explorer_request(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8 = {});

ValidatedOpenDirectory validate_open_directory_request(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8 = {});

OpenInExplorerResult open_path_in_file_manager(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8 = {},
    OpenInExplorerLauncher launcher = {});

OpenInExplorerResult open_directory_in_file_manager(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8 = {},
    DirectoryOpenLauncher launcher = {});

} // namespace acecode::desktop
