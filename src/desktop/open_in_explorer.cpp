#include "open_in_explorer.hpp"

#include "../utils/encoding.hpp"

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
#  include <shellapi.h>
#  include <windows.h>
#else
#  include <cstdlib>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {
namespace {

fs::path path_from_utf8(const std::string& text) {
#ifdef _WIN32
    return fs::path(acecode::utf8_to_wide(text));
#else
    return fs::path(text);
#endif
}

std::string compare_key(fs::path path) {
    std::error_code ec;
    path = path.lexically_normal();
    std::string value = path.generic_string();
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
        auto root = fs::weakly_canonical(path_from_utf8(root_text), ec);
        if (ec || root.empty()) continue;
        if (!fs::is_directory(root, ec) || ec) continue;
        had_valid_root = true;
        if (is_same_or_descendant(canonical_path, root)) return true;
    }
    return !had_valid_root ? false : false;
}

bool platform_open_directory(const fs::path& path, std::string& error) {
#ifdef _WIN32
    const std::wstring wide_path = path.wstring();
    HINSTANCE result = ::ShellExecuteW(nullptr, L"open", wide_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    const std::string native_path = path.string();
    pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        ::execlp(opener, opener, native_path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
#endif
}

} // namespace

ValidatedOpenDirectory validate_open_directory_request(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8) {
    if (path_utf8.empty()) {
        return {false, {}, "path required"};
    }

    std::error_code ec;
    auto requested = path_from_utf8(path_utf8);
    if (requested.empty()) {
        return {false, {}, "path required"};
    }
    if (!requested.is_absolute()) {
        return {false, {}, "path must be absolute"};
    }
    if (!fs::is_directory(requested, ec) || ec) {
        return {false, {}, "path is not an existing directory"};
    }

    auto canonical_path = fs::weakly_canonical(requested, ec);
    if (ec || canonical_path.empty()) {
        return {false, {}, "failed to resolve path"};
    }
    if (!fs::is_directory(canonical_path, ec) || ec) {
        return {false, {}, "path is not an existing directory"};
    }
    if (!is_under_allowed_root(canonical_path, allowed_roots_utf8)) {
        return {false, {}, "path is outside registered workspaces"};
    }
    return {true, canonical_path, {}};
}

OpenInExplorerResult open_directory_in_file_manager(
    const std::string& path_utf8,
    const std::vector<std::string>& allowed_roots_utf8,
    DirectoryOpenLauncher launcher) {
    auto validated = validate_open_directory_request(path_utf8, allowed_roots_utf8);
    if (!validated.ok) return {false, validated.error};

    std::string error;
    auto launch = launcher ? std::move(launcher) : DirectoryOpenLauncher(platform_open_directory);
    if (!launch(validated.path, error)) {
        if (error.empty()) error = "failed to open directory";
        return {false, error};
    }
    return {true, {}};
}

} // namespace acecode::desktop
