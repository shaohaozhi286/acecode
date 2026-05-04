#pragma once

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acecode {

// Check if a string is valid UTF-8
inline bool is_valid_utf8(const std::string& str) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(str.data());
    size_t len = str.size();
    for (size_t i = 0; i < len; ) {
        unsigned char c = bytes[i];
        int seq_len = 0;
        if (c <= 0x7F) { seq_len = 1; }
        else if ((c & 0xE0) == 0xC0) { seq_len = 2; }
        else if ((c & 0xF0) == 0xE0) { seq_len = 3; }
        else if ((c & 0xF8) == 0xF0) { seq_len = 4; }
        else { return false; }

        if (i + seq_len > len) return false;
        for (int j = 1; j < seq_len; j++) {
            if ((bytes[i + j] & 0xC0) != 0x80) return false;
        }
        i += seq_len;
    }
    return true;
}

#ifdef _WIN32
// Convert a string from the specified codepage to UTF-8
inline std::string codepage_to_utf8(const std::string& src, UINT codepage = CP_ACP) {
    if (src.empty()) return src;

    // First convert to wide string (UTF-16)
    int wide_len = MultiByteToWideChar(codepage, 0, src.c_str(), (int)src.size(), nullptr, 0);
    if (wide_len <= 0) return src;

    std::wstring wide(wide_len, L'\0');
    MultiByteToWideChar(codepage, 0, src.c_str(), (int)src.size(), &wide[0], wide_len);

    // Then convert wide string to UTF-8
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return src;

    std::string utf8(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, &utf8[0], utf8_len, nullptr, nullptr);
    return utf8;
}

inline std::wstring multibyte_to_wide(const std::string& src,
                                      UINT codepage,
                                      DWORD flags = 0) {
    if (src.empty()) return {};
    int wide_len = MultiByteToWideChar(codepage, flags, src.data(),
                                       static_cast<int>(src.size()),
                                       nullptr, 0);
    if (wide_len <= 0) return {};

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(codepage, flags, src.data(), static_cast<int>(src.size()),
                        wide.data(), wide_len);
    return wide;
}

inline std::wstring utf8_to_wide(const std::string& src) {
    if (src.empty()) return {};

    // Most ACECode persisted paths are UTF-8. Some older Windows paths came
    // from narrow filesystem APIs and are in the active codepage, so keep an
    // ACP fallback to avoid stranding existing metadata.
    std::wstring wide = multibyte_to_wide(src, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!wide.empty()) return wide;
    return multibyte_to_wide(src, CP_ACP);
}

inline std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                       static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return {};

    std::string utf8(static_cast<size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), utf8_len, nullptr, nullptr);
    return utf8;
}
#endif

// Ensure a string is valid UTF-8. On Windows, tries codepage conversion first.
// Falls back to replacing invalid bytes with '?'.
inline std::string ensure_utf8(const std::string& src) {
    if (src.empty() || is_valid_utf8(src)) return src;

#ifdef _WIN32
    // Try converting from the system's active codepage (e.g., GBK/CP936)
    std::string converted = codepage_to_utf8(src);
    if (is_valid_utf8(converted)) return converted;
#endif

    // Fallback: strip invalid bytes
    std::string result;
    result.reserve(src.size());
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(src.data());
    size_t len = src.size();
    for (size_t i = 0; i < len; ) {
        unsigned char c = bytes[i];
        int seq_len = 0;
        if (c <= 0x7F) { seq_len = 1; }
        else if ((c & 0xE0) == 0xC0) { seq_len = 2; }
        else if ((c & 0xF0) == 0xE0) { seq_len = 3; }
        else if ((c & 0xF8) == 0xF0) { seq_len = 4; }
        else { result += '?'; i++; continue; }

        if (i + seq_len > len) { result += '?'; i++; continue; }

        bool valid = true;
        for (int j = 1; j < seq_len; j++) {
            if ((bytes[i + j] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (valid) {
            result.append(src, i, seq_len);
            i += seq_len;
        } else {
            result += '?';
            i++;
        }
    }
    return result;
}

} // namespace acecode
