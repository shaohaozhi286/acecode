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

// Incremental, never-throwing decoder for subprocess output streams.
//
// Subprocess stdout/stderr arrives in arbitrary byte chunks: a single multibyte
// character (UTF-8 up to 4 bytes, GBK/DBCS 2 bytes) can be split across two
// reads. Decoding each chunk independently would corrupt the split character or
// — worse for ACECode — leave a stray byte like 0xF7 that makes nlohmann::json
// throw type_error.316 when the tool result is serialized.
//
// This is the C++ analogue of Node's StringDecoder (which Claude Code relies on
// for free), plus a Windows codepage fallback the JS runtime does not need:
//   1. prefer UTF-8 (modern toolchains, git-bash, PowerShell Core);
//   2. on Windows, fall back to the console output codepage (e.g. CP936/GBK)
//      so legacy cmd.exe output decodes to the correct characters, not '?'.
//      It holds back a trailing partial lead byte so a split DBCS pair is not
//      decoded until its second byte arrives;
//   3. last resort, replace undecodable bytes with '?' — it NEVER throws and
//      NEVER emits invalid UTF-8.
//
// Output of push()/flush() is always valid UTF-8 and safe to hand to JSON, the
// TUI, and the model context.
class IncrementalTextDecoder {
public:
    // Default: on Windows uses GetConsoleOutputCP() for the codepage fallback.
    IncrementalTextDecoder();
    // Force a specific codepage fallback (Windows only; ignored elsewhere).
    explicit IncrementalTextDecoder(unsigned int codepage);

    // Feed raw bytes; returns the decoded valid-UTF-8 text that is safe to emit
    // now. Bytes that may belong to an incomplete trailing character are kept
    // internally until the next push()/flush().
    std::string push(const char* data, size_t len);

    // Decode and return any bytes still buffered (lossy). Call once at EOF.
    std::string flush();

    // Drop buffered bytes without decoding (e.g. when reusing the decoder).
    void reset();

private:
    std::string pending_;
    unsigned int codepage_ = 0;
    bool bom_checked_ = false;
};

} // namespace acecode
