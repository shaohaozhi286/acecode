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

// Drop a trailing *incomplete* UTF-8 multi-byte sequence from a buffer. When a
// file is read with a fixed byte budget the cut can land in the middle of a
// character; left in place, that lone partial sequence makes is_valid_utf8()
// reject the whole buffer, and ensure_utf8() then re-decodes valid UTF-8 as the
// system codepage (GBK on zh-CN Windows) into mojibake. Call this on the raw
// budget-read bytes before any UTF-8 validity check. No-op when the buffer ends
// on a clean boundary (i.e. the natural end of a smaller-than-budget file).
inline void trim_trailing_partial_utf8(std::string& buf) {
    size_t cont = 0;          // trailing 10xxxxxx continuation bytes seen so far
    size_t i = buf.size();
    while (i > 0) {
        unsigned char b = static_cast<unsigned char>(buf[i - 1]);
        if ((b & 0xC0) == 0x80) {   // continuation byte: keep walking back
            if (++cont > 3) return; // more than any lead allows → malformed, leave as-is
            --i;
            continue;
        }
        size_t need = 0;            // bytes the sequence starting at b requires
        if (b < 0x80) need = 1;
        else if ((b & 0xE0) == 0xC0) need = 2;
        else if ((b & 0xF0) == 0xE0) need = 3;
        else if ((b & 0xF8) == 0xF0) need = 4;
        else return;                // invalid lead byte → let ensure_utf8() decide
        if (cont + 1 < need) buf.resize(i - 1); // trailing sequence is truncated → drop it
        return;
    }
}

// Ensure a string is valid UTF-8. On Windows, tries codepage conversion first.
// Falls back to replacing invalid bytes with '?'.
std::string ensure_utf8(const std::string& src);

} // namespace acecode
