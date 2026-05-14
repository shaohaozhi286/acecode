#include "encoding.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace acecode {

namespace {

std::string strip_utf8_bom(std::string text) {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

unsigned int read_u16(std::string_view bytes, size_t pos, bool little_endian) {
    unsigned char a = static_cast<unsigned char>(bytes[pos]);
    unsigned char b = static_cast<unsigned char>(bytes[pos + 1]);
    return little_endian
        ? (static_cast<unsigned int>(a) | (static_cast<unsigned int>(b) << 8))
        : (static_cast<unsigned int>(b) | (static_cast<unsigned int>(a) << 8));
}

std::string utf16_to_utf8(std::string_view bytes, bool little_endian, size_t offset) {
    std::string out;
    out.reserve(bytes.size() / 2);

    for (size_t i = offset; i + 1 < bytes.size(); i += 2) {
        unsigned int u = read_u16(bytes, i, little_endian);
        if (u == 0) continue;

        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes.size()) {
            unsigned int low = read_u16(bytes, i + 2, little_endian);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unsigned int cp = 0x10000 + (((u - 0xD800) << 10) | (low - 0xDC00));
                append_utf8(out, cp);
                i += 2;
                continue;
            }
        }

        if (u >= 0xD800 && u <= 0xDFFF) {
            append_utf8(out, '?');
        } else {
            append_utf8(out, u);
        }
    }

    return out;
}

bool looks_like_utf16le(std::string_view src) {
    const size_t sample = std::min<size_t>(src.size(), 512);
    if (sample < 8) return false;
    size_t odd_nuls = 0;
    size_t even_nuls = 0;
    for (size_t i = 0; i < sample; ++i) {
        if (src[i] != '\0') continue;
        if ((i % 2) == 0) ++even_nuls;
        else ++odd_nuls;
    }
    return odd_nuls > sample / 8 && even_nuls < odd_nuls / 4;
}

bool looks_like_utf16be(std::string_view src) {
    const size_t sample = std::min<size_t>(src.size(), 512);
    if (sample < 8) return false;
    size_t odd_nuls = 0;
    size_t even_nuls = 0;
    for (size_t i = 0; i < sample; ++i) {
        if (src[i] != '\0') continue;
        if ((i % 2) == 0) ++even_nuls;
        else ++odd_nuls;
    }
    return even_nuls > sample / 8 && odd_nuls < even_nuls / 4;
}

} // namespace

#ifdef _WIN32
std::string codepage_to_utf8(const std::string& src, unsigned int codepage) {
    if (src.empty()) return src;

    // First convert to wide string (UTF-16)
    int wide_len = MultiByteToWideChar(codepage, 0, src.c_str(), static_cast<int>(src.size()), nullptr, 0);
    if (wide_len <= 0) return src;

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(codepage, 0, src.c_str(), static_cast<int>(src.size()), wide.data(), wide_len);

    // Then convert wide string to UTF-8
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return src;

    std::string utf8(static_cast<size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, utf8.data(), utf8_len, nullptr, nullptr);
    return utf8;
}

namespace {

std::wstring multibyte_to_wide(const std::string& src,
                               unsigned int codepage,
                               unsigned long flags = 0) {
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

} // namespace

std::wstring utf8_to_wide(const std::string& src) {
    if (src.empty()) return {};

    // Most ACECode persisted paths are UTF-8. Some older Windows paths came
    // from narrow filesystem APIs and are in the active codepage, so keep an
    // ACP fallback to avoid stranding existing metadata.
    std::wstring wide = multibyte_to_wide(src, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!wide.empty()) return wide;
    return multibyte_to_wide(src, CP_ACP);
}

std::string wide_to_utf8(const std::wstring& wide) {
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

bool getenv_utf8(const char* name, std::string& out) {
    out.clear();
    if (!name || !*name) return false;
#ifdef _WIN32
    std::wstring wname = utf8_to_wide(name);
    if (wname.empty()) return false;

    SetLastError(ERROR_SUCCESS);
    DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed == 0) {
        return GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    }

    std::wstring value(static_cast<size_t>(needed), L'\0');
    DWORD written = GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
    if (written == 0 && GetLastError() != ERROR_SUCCESS) return false;
    value.resize(static_cast<size_t>(written));
    out = wide_to_utf8(value);
    return true;
#else
    const char* value = std::getenv(name);
    if (!value) return false;
    out = value;
    return true;
#endif
}

std::string getenv_utf8(const char* name) {
    std::string out;
    return getenv_utf8(name, out) ? out : std::string{};
}

std::string ensure_utf8(const std::string& src) {
    if (src.empty()) return {};

    if (src.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(src[0]);
        const auto b1 = static_cast<unsigned char>(src[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            return utf16_to_utf8(src, true, 2);
        }
        if (b0 == 0xFE && b1 == 0xFF) {
            return utf16_to_utf8(src, false, 2);
        }
    }

    if (looks_like_utf16le(src)) {
        return utf16_to_utf8(src, true, 0);
    }
    if (looks_like_utf16be(src)) {
        return utf16_to_utf8(src, false, 0);
    }

    if (is_valid_utf8(src)) return strip_utf8_bom(src);

#ifdef _WIN32
    // Try converting from the system's active codepage (e.g., GBK/CP936)
    std::string converted = codepage_to_utf8(src);
    if (is_valid_utf8(converted)) return strip_utf8_bom(converted);
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
    return strip_utf8_bom(result);
}

} // namespace acecode
