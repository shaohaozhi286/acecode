#include "lsp_which.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace acecode::lsp {
namespace {

namespace fs = std::filesystem;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool has_path_separator(const std::string& command) {
    return command.find('/') != std::string::npos ||
           command.find('\\') != std::string::npos;
}

// command 是否已带 pathext 中的某个扩展(大小写不敏感)。
bool ends_with_any_ext(const std::string& command,
                       const std::vector<std::string>& pathext) {
    const std::string lower = lower_ascii(command);
    for (const auto& ext : pathext) {
        const std::string lext = lower_ascii(ext);
        if (lower.size() > lext.size() &&
            lower.compare(lower.size() - lext.size(), lext.size(), lext) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> candidate_names(const std::string& command,
                                         const std::vector<std::string>& pathext) {
    if (pathext.empty() || ends_with_any_ext(command, pathext)) {
        return {command};
    }
    std::vector<std::string> names;
    // Windows 语义:先试原名(极少数无扩展可执行),再按 PATHEXT 顺序拼接。
    names.push_back(command);
    for (const auto& ext : pathext) names.push_back(command + ext);
    return names;
}

std::vector<std::string> split_list(const std::string& value, char sep) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        std::size_t next = value.find(sep, pos);
        if (next == std::string::npos) next = value.size();
        std::string item = value.substr(pos, next - pos);
        if (!item.empty()) out.push_back(std::move(item));
        pos = next + 1;
    }
    return out;
}

std::string getenv_utf8(const char* name) {
#ifdef _WIN32
    // Windows 环境变量可能含非 ACP 字符,走宽字符 API 转 UTF-8。
    std::wstring wname;
    for (const char* p = name; *p; ++p) wname.push_back(static_cast<wchar_t>(*p));
    DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
    if (written == 0 || written >= needed) return {};
    value.resize(written);
    return wide_to_utf8(value);
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

bool real_file_exists_executable(const std::string& utf8_path) {
    std::error_code ec;
    const fs::path p = path_from_utf8(utf8_path);
    if (!fs::is_regular_file(p, ec) || ec) return false;
#ifndef _WIN32
    if (::access(utf8_path.c_str(), X_OK) != 0) return false;
#endif
    return true;
}

std::string join_dir(const std::string& dir,
                     const std::string& name,
                     bool windows_semantics) {
    if (dir.empty()) return name;
    const char back = dir.back();
    if (back == '/' || back == '\\') return dir + name;
    // 跟随目录自身的分隔符风格(PATH 里两种都可能出现),避免拼出混合样式。
    if (dir.find('\\') != std::string::npos) return dir + "\\" + name;
    if (dir.find('/') != std::string::npos) return dir + "/" + name;
    return dir + (windows_semantics ? "\\" : "/") + name;
}

} // namespace

std::optional<std::string> which_in(const std::string& command,
                                    const std::vector<std::string>& path_dirs,
                                    const std::vector<std::string>& pathext,
                                    const FileExistsFn& file_exists) {
    if (command.empty()) return std::nullopt;
    const std::vector<std::string> names = candidate_names(command, pathext);

    if (has_path_separator(command)) {
        for (const auto& name : names) {
            if (file_exists(name)) return name;
        }
        return std::nullopt;
    }

    for (const auto& dir : path_dirs) {
        for (const auto& name : names) {
            const std::string full = join_dir(dir, name, !pathext.empty());
            if (file_exists(full)) return full;
        }
    }
    return std::nullopt;
}

std::optional<std::string> which(const std::string& command) {
#ifdef _WIN32
    const char sep = ';';
    std::vector<std::string> pathext = split_list(getenv_utf8("PATHEXT"), ';');
    if (pathext.empty()) pathext = {".COM", ".EXE", ".BAT", ".CMD"};
#else
    const char sep = ':';
    std::vector<std::string> pathext;
#endif
    const std::vector<std::string> path_dirs = split_list(getenv_utf8("PATH"), sep);
    return which_in(command, path_dirs, pathext, real_file_exists_executable);
}

} // namespace acecode::lsp
