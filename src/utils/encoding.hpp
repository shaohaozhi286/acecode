#pragma once

#include <string>

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
std::string codepage_to_utf8(const std::string& src, unsigned int codepage = 0);
std::wstring utf8_to_wide(const std::string& src);
std::string wide_to_utf8(const std::wstring& wide);
#endif

bool getenv_utf8(const char* name, std::string& out);
std::string getenv_utf8(const char* name);

inline std::string truncate_utf8_prefix(const std::string& src,
                                        size_t max_bytes,
                                        const std::string& suffix = "...") {
    if (src.size() <= max_bytes) return src;
    if (max_bytes <= suffix.size()) return suffix.substr(0, max_bytes);

    size_t cut = max_bytes - suffix.size();
    while (cut > 0) {
        unsigned char b = static_cast<unsigned char>(src[cut]);
        if ((b & 0xC0) != 0x80) break;
        --cut;
    }
    return src.substr(0, cut) + suffix;
}

inline std::string truncate_utf8_suffix(const std::string& src,
                                        size_t max_bytes,
                                        const std::string& prefix = "...") {
    if (src.size() <= max_bytes) return src;
    if (max_bytes <= prefix.size()) return prefix.substr(0, max_bytes);

    size_t start = src.size() - (max_bytes - prefix.size());
    while (start < src.size()) {
        unsigned char b = static_cast<unsigned char>(src[start]);
        if ((b & 0xC0) != 0x80) break;
        ++start;
    }
    return prefix + src.substr(start);
}

// Ensure a string is valid UTF-8. On Windows, tries codepage conversion first.
// Falls back to replacing invalid bytes with '?'.
std::string ensure_utf8(const std::string& src);

} // namespace acecode
