#include "open_in_explorer.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <cstdlib>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {
namespace {

std::string compare_key(fs::path path) {
    std::error_code ec;
    path = path.lexically_normal();
    std::string value = acecode::path_to_utf8_generic(path);
    while (value.size() > 1 && value.back() == '/') value.pop_back();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return value;
}

bool is_same_or_descendant(const fs::path& child, const fs::path& root) {
    const std::string child_key = compare_key(child);
    const std::string root_key = compare_key(root);
    if (child_key == root_key) return true;
    if (root_key.empty() || child_key.size() <= root_key.size()) return false;
    return child_key.compare(0, root_key.size(), root_key) == 0 &&
           child_key[root_key.size()] == '/';
}

bool is_under_allowed_root(const fs::path& canonical_path,
                           const std::vector<std::string>& allowed_roots_utf8) {
    if (allowed_roots_utf8.empty()) return true;

    bool had_valid_root = false;
    for (const auto& root_text : allowed_roots_utf8) {
        if (root_text.empty()) continue;
        std::error_code ec;
        auto root = fs::weakly_canonical(acecode::path_from_utf8(root_text), ec);
        if (ec || root.empty()) continue;
        if (!fs::is_directory(root, ec) || ec) continue;
        had_valid_root = true;
        if (is_same_or_descendant(canonical_path, root)) return true;
    }
    return !had_valid_root ? false : false;
}

bool platform_open_target(const fs::path& path,
                          OpenInExplorerTargetKind kind,
                          std::string& error) {
#ifdef _WIN32
    const std::wstring wide_path = path.wstring();
    HINSTANCE result = nullptr;
    if (kind == OpenInExplorerTargetKind::File) {
        const std::wstring parameters = L"/select,\"" + wide_path + L"\"";
        const std::wstring working_directory = path.parent_path().wstring();
        result = ::ShellExecuteW(
            nullptr,
            L"open",
            L"explorer.exe",
            parameters.c_str(),
            working_directory.c_str(),
            SW_SHOWNORMAL);
    } else {
        result = ::ShellExecuteW(
            nullptr,
            L"open",
            wide_path.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
    }
    auto code = reinterpret_cast<intptr_t>(result);
    if (code > 32) return true;
    error = "ShellExecute failed: " + std::to_string(code);
    return false;
#else
#  ifdef __APPLE__
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    const fs::path launch_path =
#  ifdef __APPLE__
        path;
#  else
        kind == OpenInExplorerTargetKind::File ? path.parent_path() : path;
#  endif
    const std::string native_path = acecode::path_to_utf8(launch_path);
    pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
#  ifdef __APPLE__
        if (kind == OpenInExplorerTargetKind::File) {
            ::execlp(opener, opener, "-R", native_path.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
#  endif
        ::execlp(opener, opener, native_path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
#endif
}

} // namespace

std::vector<std::string> append_allowed_open_root(
    std::vector<std::string> allowed_roots_utf8,
    const std::string& extra_root_utf8) {
    if (extra_root_utf8.empty()) return allowed_roots_utf8;

    const auto extra_key = compare_key(acecode::path_from_utf8(extra_root_utf8));
    if (extra_key.empty()) return allowed_roots_utf8;

    for (const auto& root : allowed_roots_utf8) {
        if (compare_key(acecode::path_from_utf8(root)) == extra_key) {
            return allowed_roots_utf8;
        }
    }
    allowed_roots_utf8.push_back(extra_root_utf8);
    return allowed_roots_utf8;
}

ValidatedOpenTarget validate_open_in_explorer_request(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8) {
    if (path_utf8.empty()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "path required"};
    }

    std::error_code ec;
    auto requested = acecode::path_from_utf8(path_utf8);
    if (requested.empty()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "path required"};
    }
    if (!requested.is_absolute()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "path must be absolute"};
    }
    const bool requested_is_directory = fs::is_directory(requested, ec);
    if (ec) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "failed to inspect path"};
    }
    const bool requested_is_file =
        !requested_is_directory && fs::is_regular_file(requested, ec);
    if (ec || (!requested_is_directory && !requested_is_file)) {
        return {
            false,
            {},
            OpenInExplorerTargetKind::Directory,
            "path is not an existing file or directory",
        };
    }

    auto canonical_path = fs::weakly_canonical(requested, ec);
    if (ec || canonical_path.empty()) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "failed to resolve path"};
    }
    const bool canonical_is_directory = fs::is_directory(canonical_path, ec);
    if (ec) {
        return {false, {}, OpenInExplorerTargetKind::Directory, "failed to inspect path"};
    }
    const bool canonical_is_file =
        !canonical_is_directory && fs::is_regular_file(canonical_path, ec);
    if (ec || (!canonical_is_directory && !canonical_is_file)) {
        return {
            false,
            {},
            OpenInExplorerTargetKind::Directory,
            "path is not an existing file or directory",
        };
    }
    if (!is_under_allowed_root(canonical_path, allowed_roots_utf8)) {
        return {
            false,
            {},
            OpenInExplorerTargetKind::Directory,
            "path is outside registered workspaces",
        };
    }
    return {
        true,
        canonical_path,
        canonical_is_file
            ? OpenInExplorerTargetKind::File
            : OpenInExplorerTargetKind::Directory,
        {},
    };
}

ValidatedOpenDirectory validate_open_directory_request(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8) {
    auto validated = validate_open_in_explorer_request(
        path_utf8,
        allowed_roots_utf8);
    if (!validated.ok) return {false, {}, validated.error};
    if (validated.kind != OpenInExplorerTargetKind::Directory) {
        return {false, {}, "path is not an existing directory"};
    }
    return {true, std::move(validated.path), {}};
}

OpenInExplorerResult open_path_in_file_manager(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8,
    OpenInExplorerLauncher launcher) {
    auto validated = validate_open_in_explorer_request(
        path_utf8,
        allowed_roots_utf8);
    if (!validated.ok) return {false, validated.error};

    std::string error;
    auto launch = launcher
        ? std::move(launcher)
        : OpenInExplorerLauncher(platform_open_target);
    if (!launch(validated.path, validated.kind, error)) {
        if (error.empty()) error = "failed to open path in file manager";
        return {false, error};
    }
    return {true, {}};
}

OpenInExplorerResult open_directory_in_file_manager(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8,
    DirectoryOpenLauncher launcher) {
    auto validated = validate_open_directory_request(path_utf8, allowed_roots_utf8);
    if (!validated.ok) return {false, validated.error};

    std::string error;
    auto launch = launcher
        ? std::move(launcher)
        : DirectoryOpenLauncher([](const fs::path& path, std::string& launch_error) {
            return platform_open_target(
                path,
                OpenInExplorerTargetKind::Directory,
                launch_error);
        });
    if (!launch(validated.path, error)) {
        if (error.empty()) error = "failed to open directory";
        return {false, error};
    }
    return {true, {}};
}

} // namespace acecode::desktop
