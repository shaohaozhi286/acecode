#pragma once

#include "encoding.hpp"

#include <filesystem>
#include <string>
#include <system_error>

namespace acecode {

inline std::filesystem::path path_from_utf8(const std::string& text) {
#ifdef _WIN32
    return std::filesystem::path(utf8_to_wide(text));
#else
    return std::filesystem::path(text);
#endif
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    return wide_to_utf8(path.wstring());
#else
    return path.string();
#endif
}

inline std::string path_to_utf8_generic(const std::filesystem::path& path) {
#ifdef _WIN32
    return wide_to_utf8(path.generic_wstring());
#else
    return path.generic_string();
#endif
}

inline std::string current_path_utf8() {
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    if (ec) return {};
    return path_to_utf8(path);
}

} // namespace acecode
