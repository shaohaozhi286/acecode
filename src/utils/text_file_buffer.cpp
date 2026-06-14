#include "text_file_buffer.hpp"

#include "sha256.hpp"
#include "tool_errors.hpp"
#include "utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

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

constexpr unsigned int kCodePageGbk = 936;
constexpr unsigned int kCodePageGb18030 = 54936;
constexpr size_t kStrongUtf8InvalidByteCap = 4;

struct Utf8ScanStats {
    size_t total_bytes = 0;
    size_t valid_non_ascii_codepoints = 0;
    size_t invalid_bytes = 0;
    size_t invalid_spans = 0;
};

struct LossyDecodeCandidate {
    bool success = true;
    std::string utf8;
    TextEncoding encoding = TextEncoding::Utf8;
    size_t replacement_count = 0;
    int preference = 0;
};

bool starts_with_bom(const std::string& bytes,
                     unsigned char a,
                     unsigned char b,
                     unsigned char c = 0,
                     size_t len = 2) {
    if (bytes.size() < len) return false;
    if (static_cast<unsigned char>(bytes[0]) != a) return false;
    if (static_cast<unsigned char>(bytes[1]) != b) return false;
    return len == 2 || static_cast<unsigned char>(bytes[2]) == c;
}

bool read_raw_file(const std::string& path, std::string& out, std::string& error) {
    std::ifstream ifs(path_from_utf8(path), std::ios::binary);
    if (!ifs.is_open()) {
        error = ToolErrors::cannot_open_file(path);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

bool write_raw_file(const std::string& path, const std::string& bytes, std::string& error) {
    auto fs_path = path_from_utf8(path);
    auto parent = fs_path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        try {
            std::filesystem::create_directories(parent);
        } catch (const std::filesystem::filesystem_error& e) {
            error = "[Error] Cannot create parent directory: " + std::string(e.what());
            return false;
        }
    }

    std::ofstream ofs(fs_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        error = ToolErrors::cannot_write_file(path);
        return false;
    }
    ofs.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!ofs.good()) {
        error = ToolErrors::cannot_write_file(path);
        return false;
    }
    return true;
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

unsigned int read_u16(const std::string& bytes, size_t pos, bool little_endian) {
    const auto a = static_cast<unsigned char>(bytes[pos]);
    const auto b = static_cast<unsigned char>(bytes[pos + 1]);
    return little_endian
        ? (static_cast<unsigned int>(a) | (static_cast<unsigned int>(b) << 8))
        : (static_cast<unsigned int>(b) | (static_cast<unsigned int>(a) << 8));
}

void append_u16(std::string& out, unsigned int value, bool little_endian) {
    if (little_endian) {
        out.push_back(static_cast<char>(value & 0xFF));
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
    } else {
        out.push_back(static_cast<char>((value >> 8) & 0xFF));
        out.push_back(static_cast<char>(value & 0xFF));
    }
}

TextEncodeResult decode_utf16_bytes(const std::string& bytes,
                                    bool little_endian,
                                    size_t offset) {
    if (((bytes.size() - offset) % 2) != 0) {
        return {false, {}, "[Error] UTF-16 text has an odd byte count."};
    }

    std::string out;
    out.reserve(bytes.size() / 2);
    for (size_t i = offset; i + 1 < bytes.size(); i += 2) {
        unsigned int u = read_u16(bytes, i, little_endian);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 3 >= bytes.size()) {
                return {false, {}, "[Error] UTF-16 text ended inside a surrogate pair."};
            }
            unsigned int low = read_u16(bytes, i + 2, little_endian);
            if (low < 0xDC00 || low > 0xDFFF) {
                return {false, {}, "[Error] UTF-16 text contains an invalid surrogate pair."};
            }
            unsigned int cp = 0x10000 + (((u - 0xD800) << 10) | (low - 0xDC00));
            append_utf8(out, cp);
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            return {false, {}, "[Error] UTF-16 text contains an unpaired low surrogate."};
        } else {
            append_utf8(out, u);
        }
    }
    return {true, out, {}};
}

bool next_utf8_codepoint(const std::string& s, size_t& i, unsigned int& cp) {
    if (i >= s.size()) return false;
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 <= 0x7F) {
        cp = b0;
        ++i;
        return true;
    }

    int len = 0;
    unsigned int value = 0;
    unsigned int min_value = 0;
    if ((b0 & 0xE0) == 0xC0) {
        len = 2; value = b0 & 0x1F; min_value = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3; value = b0 & 0x0F; min_value = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4; value = b0 & 0x07; min_value = 0x10000;
    } else {
        return false;
    }

    if (i + static_cast<size_t>(len) > s.size()) return false;
    for (int j = 1; j < len; ++j) {
        const auto b = static_cast<unsigned char>(s[i + j]);
        if ((b & 0xC0) != 0x80) return false;
        value = (value << 6) | (b & 0x3F);
    }
    if (value < min_value || value > 0x10FFFF) return false;
    if (value >= 0xD800 && value <= 0xDFFF) return false;
    cp = value;
    i += static_cast<size_t>(len);
    return true;
}

Utf8ScanStats scan_utf8_bytes(const std::string& bytes) {
    Utf8ScanStats stats;
    stats.total_bytes = bytes.size();
    for (size_t i = 0; i < bytes.size();) {
        const size_t start = i;
        unsigned int cp = 0;
        if (next_utf8_codepoint(bytes, i, cp)) {
            if (cp > 0x7F) {
                ++stats.valid_non_ascii_codepoints;
            }
            continue;
        }
        ++stats.invalid_bytes;
        ++stats.invalid_spans;
        i = start + 1;
    }
    return stats;
}

bool strongly_looks_like_damaged_utf8(const Utf8ScanStats& stats, bool has_utf8_bom) {
    if (stats.invalid_bytes == 0) return false;
    if (!has_utf8_bom && stats.valid_non_ascii_codepoints == 0) return false;
    if (!has_utf8_bom && stats.valid_non_ascii_codepoints < stats.invalid_spans) {
        return false;
    }
    const bool small_absolute_damage = stats.invalid_bytes <= kStrongUtf8InvalidByteCap;
    const size_t total = std::max<size_t>(stats.total_bytes, 1);
    const bool small_ratio_damage = stats.invalid_bytes * 100 <= total;
    return small_absolute_damage || small_ratio_damage;
}

LossyDecodeCandidate decode_lossy_utf8(const std::string& bytes,
                                       TextEncoding encoding = TextEncoding::Utf8,
                                       int preference = 0) {
    static const std::string kReplacementUtf8 = "\xEF\xBF\xBD";
    LossyDecodeCandidate candidate;
    candidate.encoding = encoding;
    candidate.preference = preference;
    candidate.utf8.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size();) {
        const size_t start = i;
        unsigned int cp = 0;
        if (next_utf8_codepoint(bytes, i, cp)) {
            candidate.utf8.append(bytes, start, i - start);
            continue;
        }
        candidate.utf8 += kReplacementUtf8;
        ++candidate.replacement_count;
        i = start + 1;
    }
    return candidate;
}

TextEncodeResult encode_utf16_bytes(const std::string& utf8,
                                    bool little_endian,
                                    bool with_bom) {
    if (!text_bytes_are_valid_utf8(utf8)) {
        return {false, {}, "[Error] Internal text is not valid UTF-8."};
    }

    std::string out;
    if (with_bom) {
        if (little_endian) {
            out.push_back(static_cast<char>(0xFF));
            out.push_back(static_cast<char>(0xFE));
        } else {
            out.push_back(static_cast<char>(0xFE));
            out.push_back(static_cast<char>(0xFF));
        }
    }

    for (size_t i = 0; i < utf8.size();) {
        unsigned int cp = 0;
        if (!next_utf8_codepoint(utf8, i, cp)) {
            return {false, {}, "[Error] Internal text is not valid UTF-8."};
        }
        if (cp <= 0xFFFF) {
            append_u16(out, cp, little_endian);
        } else {
            cp -= 0x10000;
            append_u16(out, 0xD800 | (cp >> 10), little_endian);
            append_u16(out, 0xDC00 | (cp & 0x3FF), little_endian);
        }
    }
    return {true, out, {}};
}

bool looks_like_binary(const std::string& bytes) {
    if (bytes.empty()) return false;
    const size_t sample = std::min<size_t>(bytes.size(), 4096);
    size_t controls = 0;
    for (size_t i = 0; i < sample; ++i) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0) return true;
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' &&
            c != '\f' && c != '\b' && c != 0x1B) {
            ++controls;
        }
    }
    return controls > sample / 20;
}

bool looks_like_utf16le_without_bom(const std::string& bytes) {
    const size_t sample = std::min<size_t>(bytes.size(), 512);
    if (sample < 8) return false;
    size_t odd_nuls = 0;
    size_t even_nuls = 0;
    for (size_t i = 0; i < sample; ++i) {
        if (bytes[i] != '\0') continue;
        ((i % 2) ? odd_nuls : even_nuls)++;
    }
    return odd_nuls > sample / 8 && even_nuls < odd_nuls / 4;
}

bool looks_like_utf16be_without_bom(const std::string& bytes) {
    const size_t sample = std::min<size_t>(bytes.size(), 512);
    if (sample < 8) return false;
    size_t odd_nuls = 0;
    size_t even_nuls = 0;
    for (size_t i = 0; i < sample; ++i) {
        if (bytes[i] != '\0') continue;
        ((i % 2) ? odd_nuls : even_nuls)++;
    }
    return even_nuls > sample / 8 && odd_nuls < even_nuls / 4;
}

#ifdef _WIN32
TextEncodeResult decode_codepage(const std::string& bytes,
                                 unsigned int codepage,
                                 TextEncoding target_encoding) {
    if (bytes.empty()) return {true, {}, {}};
    int wide_len = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS,
                                       bytes.data(), static_cast<int>(bytes.size()),
                                       nullptr, 0);
    if (wide_len <= 0) {
        return {false, {}, "[Error] Cannot decode file as " + text_encoding_label(target_encoding) + "."};
    }

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS,
                            bytes.data(), static_cast<int>(bytes.size()),
                            wide.data(), wide_len) <= 0) {
        return {false, {}, "[Error] Cannot decode file as " + text_encoding_label(target_encoding) + "."};
    }

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0,
                                       wide.data(), wide_len,
                                       nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        return {false, {}, "[Error] Cannot convert " + text_encoding_label(target_encoding) + " text to UTF-8."};
    }

    std::string utf8(static_cast<size_t>(utf8_len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0,
                            wide.data(), wide_len,
                            utf8.data(), utf8_len,
                            nullptr, nullptr) <= 0) {
        return {false, {}, "[Error] Cannot convert " + text_encoding_label(target_encoding) + " text to UTF-8."};
    }
    return {true, utf8, {}};
}

LossyDecodeCandidate decode_lossy_codepage(const std::string& bytes,
                                           unsigned int codepage,
                                           TextEncoding target_encoding,
                                           int preference) {
    LossyDecodeCandidate candidate;
    candidate.encoding = target_encoding;
    candidate.preference = preference;
    if (bytes.empty()) return candidate;

    const bool strict_success =
        MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS,
                            bytes.data(), static_cast<int>(bytes.size()),
                            nullptr, 0) > 0;

    int wide_len = MultiByteToWideChar(codepage, 0,
                                       bytes.data(), static_cast<int>(bytes.size()),
                                       nullptr, 0);
    if (wide_len <= 0) {
        candidate.success = false;
        return candidate;
    }

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(codepage, 0,
                            bytes.data(), static_cast<int>(bytes.size()),
                            wide.data(), wide_len) <= 0) {
        candidate.success = false;
        return candidate;
    }
    for (wchar_t ch : wide) {
        if (ch == static_cast<wchar_t>(0xFFFD)) {
            ++candidate.replacement_count;
        }
    }
    if (!strict_success && candidate.replacement_count == 0) {
        candidate.replacement_count = 1;
    }

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0,
                                       wide.data(), wide_len,
                                       nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        candidate.success = false;
        return candidate;
    }

    candidate.utf8.assign(static_cast<size_t>(utf8_len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0,
                            wide.data(), wide_len,
                            candidate.utf8.data(), utf8_len,
                            nullptr, nullptr) <= 0) {
        candidate.success = false;
    }
    return candidate;
}

TextEncodeResult encode_codepage(const std::string& utf8,
                                 unsigned int codepage,
                                 TextEncoding target_encoding) {
    if (utf8.empty()) return {true, {}, {}};
    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       utf8.data(), static_cast<int>(utf8.size()),
                                       nullptr, 0);
    if (wide_len <= 0) {
        return {false, {}, "[Error] Replacement text is not valid UTF-8."};
    }

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            utf8.data(), static_cast<int>(utf8.size()),
                            wide.data(), wide_len) <= 0) {
        return {false, {}, "[Error] Replacement text is not valid UTF-8."};
    }

    BOOL used_default = FALSE;
    DWORD flags = 0;
    if (codepage != kCodePageGb18030) {
        flags = WC_NO_BEST_FIT_CHARS;
    }
    int byte_len = WideCharToMultiByte(codepage, flags,
                                       wide.data(), wide_len,
                                       nullptr, 0, nullptr,
                                       codepage == kCodePageGb18030 ? nullptr : &used_default);
    if (byte_len <= 0 || used_default) {
        return {false, {},
                "[Error] Text cannot be represented in target encoding " +
                text_encoding_label(target_encoding) +
                ". Use an explicit UTF-8 conversion before writing."};
    }

    std::string bytes(static_cast<size_t>(byte_len), '\0');
    used_default = FALSE;
    if (WideCharToMultiByte(codepage, flags,
                            wide.data(), wide_len,
                            bytes.data(), byte_len, nullptr,
                            codepage == kCodePageGb18030 ? nullptr : &used_default) <= 0 ||
        used_default) {
        return {false, {},
                "[Error] Text cannot be represented in target encoding " +
                text_encoding_label(target_encoding) +
                ". Use an explicit UTF-8 conversion before writing."};
    }
    return {true, bytes, {}};
}
#endif

LossyDecodeCandidate decode_lossy_best_fit(const std::string& bytes) {
    std::vector<LossyDecodeCandidate> candidates;
    candidates.push_back(decode_lossy_utf8(bytes, TextEncoding::Utf8, 0));
#ifdef _WIN32
    auto gbk = decode_lossy_codepage(bytes, kCodePageGbk, TextEncoding::Gbk, 1);
    if (gbk.success) candidates.push_back(std::move(gbk));
    auto gb18030 = decode_lossy_codepage(bytes, kCodePageGb18030, TextEncoding::Gb18030, 2);
    if (gb18030.success) candidates.push_back(std::move(gb18030));
#endif

    return *std::min_element(
        candidates.begin(), candidates.end(),
        [](const LossyDecodeCandidate& a, const LossyDecodeCandidate& b) {
            if (a.replacement_count != b.replacement_count) {
                return a.replacement_count < b.replacement_count;
            }
            return a.preference < b.preference;
        });
}

TextBufferResult make_decoded_result(const std::string& path,
                                     const std::string& raw,
                                     std::string decoded_utf8,
                                     TextEncoding encoding,
                                     bool has_bom,
                                     bool lossy = false,
                                     size_t lossy_replacement_count = 0) {
    TextFileMetadata metadata;
    metadata.encoding = encoding;
    metadata.has_bom = has_bom;
    metadata.lossy = lossy;
    metadata.lossy_replacement_count = lossy ? lossy_replacement_count : 0;
    metadata.line_ending = detect_line_ending_style(decoded_utf8);
    TextFileBuffer buffer;
    buffer.path = path;
    buffer.raw_bytes = raw;
    buffer.text = normalize_text_to_lf(std::move(decoded_utf8));
    buffer.metadata = metadata;
    return {true, std::move(buffer), {}};
}

TextBufferResult fail_decode(const std::string& path,
                             const std::string& raw,
                             TextEncoding encoding,
                             const std::string& error,
                             bool binary = false) {
    TextFileMetadata metadata;
    metadata.encoding = encoding;
    metadata.binary = binary;
    metadata.unsupported = !binary;
    metadata.error = error;

    TextFileBuffer buffer;
    buffer.path = path;
    buffer.raw_bytes = raw;
    buffer.metadata = metadata;
    return {false, std::move(buffer), error};
}

LineEndingStyle write_style_for(LineEndingStyle style) {
    if (style == LineEndingStyle::CrLf || style == LineEndingStyle::Cr) return style;
    return LineEndingStyle::Lf;
}

} // namespace

std::string text_encoding_label(TextEncoding encoding) {
    switch (encoding) {
        case TextEncoding::Utf8: return "utf-8";
        case TextEncoding::Utf8Bom: return "utf-8-bom";
        case TextEncoding::Utf16Le: return "utf-16le";
        case TextEncoding::Utf16Be: return "utf-16be";
        case TextEncoding::Gbk: return "gbk/cp936";
        case TextEncoding::Gb18030: return "gb18030";
        case TextEncoding::Binary: return "binary";
        case TextEncoding::Unsupported: return "unsupported";
    }
    return "unknown";
}

std::string line_ending_label(LineEndingStyle style) {
    switch (style) {
        case LineEndingStyle::None: return "none";
        case LineEndingStyle::Lf: return "lf";
        case LineEndingStyle::CrLf: return "crlf";
        case LineEndingStyle::Cr: return "cr";
        case LineEndingStyle::Mixed: return "mixed";
    }
    return "unknown";
}

bool text_bytes_are_valid_utf8(const std::string& bytes) {
    for (size_t i = 0; i < bytes.size();) {
        unsigned int cp = 0;
        if (!next_utf8_codepoint(bytes, i, cp)) return false;
    }
    return true;
}

std::string normalize_text_to_lf(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            out.push_back('\n');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

LineEndingStyle detect_line_ending_style(const std::string& text) {
    size_t crlf = 0;
    size_t lf = 0;
    size_t cr = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++crlf;
                ++i;
            } else {
                ++cr;
            }
        } else if (text[i] == '\n') {
            ++lf;
        }
    }

    const size_t kinds = (crlf > 0 ? 1 : 0) + (lf > 0 ? 1 : 0) + (cr > 0 ? 1 : 0);
    if (kinds == 0) return LineEndingStyle::None;
    if (kinds > 1) return LineEndingStyle::Mixed;
    if (crlf > 0) return LineEndingStyle::CrLf;
    if (cr > 0) return LineEndingStyle::Cr;
    return LineEndingStyle::Lf;
}

std::string restore_line_endings(const std::string& lf_text, LineEndingStyle style) {
    const LineEndingStyle target = write_style_for(style);
    if (target == LineEndingStyle::Lf) return normalize_text_to_lf(lf_text);

    std::string normalized = normalize_text_to_lf(lf_text);
    std::string out;
    out.reserve(normalized.size() + normalized.size() / 16);
    const char* replacement = target == LineEndingStyle::CrLf ? "\r\n" : "\r";
    for (char c : normalized) {
        if (c == '\n') out += replacement;
        else out.push_back(c);
    }
    return out;
}

TextBufferResult decode_text_file_bytes(const std::string& bytes,
                                        const std::string& path,
                                        bool allow_lossy) {
    if (starts_with_bom(bytes, 0xEF, 0xBB, 0xBF, 3)) {
        std::string body = bytes.substr(3);
        if (!text_bytes_are_valid_utf8(body)) {
            if (allow_lossy && !looks_like_binary(body)) {
                auto lossy = decode_lossy_utf8(body, TextEncoding::Utf8Bom, 0);
                return make_decoded_result(path, bytes, std::move(lossy.utf8),
                                           TextEncoding::Utf8Bom, true, true,
                                           lossy.replacement_count);
            }
            return fail_decode(path, bytes, TextEncoding::Unsupported,
                "[Error] UTF-8 BOM file can be read with file_read using lossy decoding, but its bytes are too ambiguous to edit safely.");
        }
        return make_decoded_result(path, bytes, body, TextEncoding::Utf8Bom, true);
    }

    if (starts_with_bom(bytes, 0xFF, 0xFE)) {
        auto decoded = decode_utf16_bytes(bytes, true, 2);
        if (!decoded.success) {
            return fail_decode(path, bytes, TextEncoding::Unsupported, decoded.error);
        }
        return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Utf16Le, true);
    }

    if (starts_with_bom(bytes, 0xFE, 0xFF)) {
        auto decoded = decode_utf16_bytes(bytes, false, 2);
        if (!decoded.success) {
            return fail_decode(path, bytes, TextEncoding::Unsupported, decoded.error);
        }
        return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Utf16Be, true);
    }

    if (looks_like_utf16le_without_bom(bytes)) {
        auto decoded = decode_utf16_bytes(bytes, true, 0);
        if (decoded.success) {
            return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Utf16Le, false);
        }
    }
    if (looks_like_utf16be_without_bom(bytes)) {
        auto decoded = decode_utf16_bytes(bytes, false, 0);
        if (decoded.success) {
            return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Utf16Be, false);
        }
    }

    if (looks_like_binary(bytes)) {
        return fail_decode(path, bytes, TextEncoding::Binary,
            "[Error] File appears to be binary and cannot be safely edited as text.",
            true);
    }

    if (text_bytes_are_valid_utf8(bytes)) {
        return make_decoded_result(path, bytes, bytes, TextEncoding::Utf8, false);
    }

    const Utf8ScanStats utf8_stats = scan_utf8_bytes(bytes);
    if (strongly_looks_like_damaged_utf8(utf8_stats, false)) {
        if (allow_lossy) {
            auto lossy = decode_lossy_utf8(bytes, TextEncoding::Utf8, 0);
            return make_decoded_result(path, bytes, std::move(lossy.utf8),
                                       TextEncoding::Utf8, false, true,
                                       lossy.replacement_count);
        }
        return fail_decode(path, bytes, TextEncoding::Unsupported,
            "[Error] File can be read with file_read using lossy UTF-8 decoding, but its encoding is too ambiguous to edit safely.");
    }

#ifdef _WIN32
    auto gbk = decode_codepage(bytes, kCodePageGbk, TextEncoding::Gbk);
    if (gbk.success && text_bytes_are_valid_utf8(gbk.bytes)) {
        return make_decoded_result(path, bytes, gbk.bytes, TextEncoding::Gbk, false);
    }

    auto gb18030 = decode_codepage(bytes, kCodePageGb18030, TextEncoding::Gb18030);
    if (gb18030.success && text_bytes_are_valid_utf8(gb18030.bytes)) {
        return make_decoded_result(path, bytes, gb18030.bytes, TextEncoding::Gb18030, false);
    }
#endif

    if (allow_lossy) {
        auto lossy = decode_lossy_best_fit(bytes);
        return make_decoded_result(path, bytes, std::move(lossy.utf8),
                                   lossy.encoding, false, true,
                                   lossy.replacement_count);
    }

    return fail_decode(path, bytes, TextEncoding::Unsupported,
        "[Error] File can be read with file_read using lossy decoding, but its encoding is too ambiguous to edit safely.");
}

TextBufferResult decode_text_file_bytes_with_metadata(const std::string& bytes,
                                                      const TextFileMetadata& metadata,
                                                      const std::string& path) {
    switch (metadata.encoding) {
        case TextEncoding::Utf8:
            if (!text_bytes_are_valid_utf8(bytes)) {
                return fail_decode(path, bytes, TextEncoding::Unsupported,
                    "[Error] Written bytes are not valid UTF-8.");
            }
            return make_decoded_result(path, bytes, bytes, TextEncoding::Utf8, false);
        case TextEncoding::Utf8Bom:
            if (!starts_with_bom(bytes, 0xEF, 0xBB, 0xBF, 3)) {
                return fail_decode(path, bytes, TextEncoding::Unsupported,
                    "[Error] UTF-8 BOM was not preserved.");
            }
            return decode_text_file_bytes(bytes, path);
        case TextEncoding::Utf16Le: {
            size_t offset = starts_with_bom(bytes, 0xFF, 0xFE) ? 2 : 0;
            auto decoded = decode_utf16_bytes(bytes, true, offset);
            if (!decoded.success) return fail_decode(path, bytes, TextEncoding::Unsupported, decoded.error);
            return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Utf16Le, offset == 2);
        }
        case TextEncoding::Utf16Be: {
            size_t offset = starts_with_bom(bytes, 0xFE, 0xFF) ? 2 : 0;
            auto decoded = decode_utf16_bytes(bytes, false, offset);
            if (!decoded.success) return fail_decode(path, bytes, TextEncoding::Unsupported, decoded.error);
            return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Utf16Be, offset == 2);
        }
        case TextEncoding::Gbk:
#ifdef _WIN32
        {
            auto decoded = decode_codepage(bytes, kCodePageGbk, TextEncoding::Gbk);
            if (!decoded.success) return fail_decode(path, bytes, TextEncoding::Unsupported, decoded.error);
            return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Gbk, false);
        }
#else
            break;
#endif
        case TextEncoding::Gb18030:
#ifdef _WIN32
        {
            auto decoded = decode_codepage(bytes, kCodePageGb18030, TextEncoding::Gb18030);
            if (!decoded.success) return fail_decode(path, bytes, TextEncoding::Unsupported, decoded.error);
            return make_decoded_result(path, bytes, decoded.bytes, TextEncoding::Gb18030, false);
        }
#else
            break;
#endif
        case TextEncoding::Binary:
        case TextEncoding::Unsupported:
            break;
    }
    return fail_decode(path, bytes, TextEncoding::Unsupported,
        "[Error] Cannot verify bytes with unsupported target encoding.");
}

TextBufferResult read_text_file_buffer(const std::string& path, bool allow_lossy) {
    std::string raw;
    std::string error;
    if (!read_raw_file(path, raw, error)) {
        return fail_decode(path, raw, TextEncoding::Unsupported, error);
    }
    return decode_text_file_bytes(raw, path, allow_lossy);
}

TextEncodeResult encode_text_for_write(const std::string& lf_text,
                                       const TextFileMetadata& metadata) {
    if (metadata.lossy) {
        return {false, {},
            "[Error] Target file cannot be safely written as text because it was decoded lossily. Use file_read for inspection and convert the file to a confirmed encoding before editing."};
    }
    if (metadata.binary || metadata.unsupported ||
        metadata.encoding == TextEncoding::Binary ||
        metadata.encoding == TextEncoding::Unsupported) {
        return {false, {},
            "[Error] Target file cannot be safely written as text because its encoding is " +
            text_encoding_label(metadata.encoding) + "."};
    }

    const std::string text_with_original_eol =
        restore_line_endings(lf_text, metadata.line_ending);

    switch (metadata.encoding) {
        case TextEncoding::Utf8:
            if (!text_bytes_are_valid_utf8(text_with_original_eol)) {
                return {false, {}, "[Error] Text is not valid UTF-8."};
            }
            return {true, text_with_original_eol, {}};
        case TextEncoding::Utf8Bom: {
            if (!text_bytes_are_valid_utf8(text_with_original_eol)) {
                return {false, {}, "[Error] Text is not valid UTF-8."};
            }
            std::string bytes;
            bytes.push_back(static_cast<char>(0xEF));
            bytes.push_back(static_cast<char>(0xBB));
            bytes.push_back(static_cast<char>(0xBF));
            bytes += text_with_original_eol;
            return {true, bytes, {}};
        }
        case TextEncoding::Utf16Le:
            return encode_utf16_bytes(text_with_original_eol, true, metadata.has_bom);
        case TextEncoding::Utf16Be:
            return encode_utf16_bytes(text_with_original_eol, false, metadata.has_bom);
        case TextEncoding::Gbk:
#ifdef _WIN32
            return encode_codepage(text_with_original_eol, kCodePageGbk, TextEncoding::Gbk);
#else
            return {false, {}, "[Error] GBK encoding is only available through the Windows codepage adapter in this build."};
#endif
        case TextEncoding::Gb18030:
#ifdef _WIN32
            return encode_codepage(text_with_original_eol, kCodePageGb18030, TextEncoding::Gb18030);
#else
            return {false, {}, "[Error] GB18030 encoding is only available through the Windows codepage adapter in this build."};
#endif
        case TextEncoding::Binary:
        case TextEncoding::Unsupported:
            break;
    }

    return {false, {}, "[Error] Unsupported target text encoding."};
}

TextFileMetadata default_new_file_text_metadata() {
    TextFileMetadata metadata;
    metadata.encoding = TextEncoding::Utf8;
    metadata.line_ending = LineEndingStyle::Lf;
    metadata.has_bom = false;
    return metadata;
}

TextSafeWriteResult safe_write_text_file(
    const std::string& path,
    const std::string& lf_text,
    const TextFileMetadata& metadata,
    const std::function<void(const std::string& path)>& before_write) {
    auto encoded = encode_text_for_write(lf_text, metadata);
    if (!encoded.success) {
        return {false, encoded.error, false, false};
    }

    std::string pre_write_bytes;
    std::string error;
    const bool existed = std::filesystem::exists(path_from_utf8(path));
    if (existed && !read_raw_file(path, pre_write_bytes, error)) {
        return {false, error, false, false};
    }

    if (before_write) {
        before_write(path);
    }

    if (!write_raw_file(path, encoded.bytes, error)) {
        return {false, error, false, false};
    }

    std::string written_bytes;
    if (!read_raw_file(path, written_bytes, error)) {
        if (existed && !write_raw_file(path, pre_write_bytes, error)) {
            return {false,
                "[Error] Post-write verification could not read " + path +
                " and rollback failed: " + error,
                false,
                true};
        }
        return {false,
            "[Error] Post-write verification could not read " + path +
            "; edit was rolled back.",
            true,
            false};
    }

    auto round_trip = decode_text_file_bytes_with_metadata(written_bytes, metadata, path);
    if (round_trip.success && round_trip.buffer.text == normalize_text_to_lf(lf_text)) {
        return {true, {}, false, false};
    }

    std::string rollback_error;
    if (existed) {
        if (!write_raw_file(path, pre_write_bytes, rollback_error)) {
            return {false,
                "[Error] Post-write round-trip verification failed for " + path +
                " and rollback failed: " + rollback_error,
                false,
                true};
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(path_from_utf8(path), ec);
        if (ec) {
            return {false,
                "[Error] Post-write round-trip verification failed for " + path +
                " and rollback failed: " + ec.message(),
                false,
                true};
        }
    }

    std::string reason = round_trip.success
        ? "[Error] Post-write round-trip verification changed the intended text"
        : round_trip.error;
    return {false,
        reason + "; edit was rolled back for " + path + ".",
        true,
        false};
}

std::vector<std::string> split_lf_lines_preserve_empty(const std::string& lf_text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= lf_text.size()) {
        size_t pos = lf_text.find('\n', start);
        if (pos == std::string::npos) {
            if (start < lf_text.size()) lines.push_back(lf_text.substr(start));
            break;
        }
        lines.push_back(lf_text.substr(start, pos - start));
        start = pos + 1;
        if (start == lf_text.size()) break;
    }
    return lines;
}

std::string line_range_content(const std::string& lf_text,
                               int start_line,
                               int end_line,
                               bool* ok,
                               int* total_lines) {
    if (ok) *ok = false;
    if (total_lines) *total_lines = 0;
    if (start_line <= 0 || end_line < start_line) return {};

    std::vector<size_t> starts;
    starts.push_back(0);
    for (size_t i = 0; i < lf_text.size(); ++i) {
        if (lf_text[i] == '\n' && i + 1 < lf_text.size()) {
            starts.push_back(i + 1);
        }
    }

    const int line_count = lf_text.empty() ? 0 : static_cast<int>(starts.size());
    if (total_lines) *total_lines = line_count;
    if (line_count == 0 || start_line > line_count) return {};

    int clamped_end = std::min(end_line, line_count);
    const size_t start_offset = starts[static_cast<size_t>(start_line - 1)];
    size_t end_offset = lf_text.size();
    if (clamped_end < line_count) {
        end_offset = starts[static_cast<size_t>(clamped_end)] ;
    }

    if (ok) *ok = true;
    return lf_text.substr(start_offset, end_offset - start_offset);
}

std::string range_hash(const std::string& lf_text, int start_line, int end_line) {
    bool ok = false;
    std::string content = line_range_content(lf_text, start_line, end_line, &ok, nullptr);
    if (!ok) return {};
    return "sha256:" + sha256_hex(content).substr(0, 16);
}

std::string read_id_for_text_buffer(const std::string& path, const std::string& raw_bytes) {
    return sha256_hex(path + "\0" + raw_bytes).substr(0, 16);
}

} // namespace acecode
