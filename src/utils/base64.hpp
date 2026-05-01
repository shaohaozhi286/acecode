#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace acecode {

// Standard base64 encoder. Alphabet A-Za-z0-9+/, pads to 4-byte boundary with '='.
// Accepts arbitrary bytes (the argument is treated as raw bytes, not text).
// Header-only; no decoder in this release.
//
// Test vectors verified while writing:
//   ""            -> ""
//   "M"           -> "TQ=="
//   "Ma"          -> "TWE="
//   "Man"         -> "TWFu"
//   "\x00\xff\x10" -> "AP8Q"
inline std::string base64_encode(std::string_view data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const size_t n = data.size();
    if (n == 0) return std::string();

    std::string out;
    out.resize(((n + 2) / 3) * 4);

    size_t o = 0;
    size_t i = 0;
    // Process full 3-byte groups.
    for (; i + 3 <= n; i += 3) {
        const uint32_t triple =
            (static_cast<uint8_t>(data[i])     << 16) |
            (static_cast<uint8_t>(data[i + 1]) <<  8) |
            (static_cast<uint8_t>(data[i + 2]));
        out[o++] = kAlphabet[(triple >> 18) & 0x3F];
        out[o++] = kAlphabet[(triple >> 12) & 0x3F];
        out[o++] = kAlphabet[(triple >>  6) & 0x3F];
        out[o++] = kAlphabet[(triple      ) & 0x3F];
    }
    // Tail: 0, 1 or 2 bytes remaining.
    const size_t rem = n - i;
    if (rem == 1) {
        const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        out[o++] = kAlphabet[(v >> 18) & 0x3F];
        out[o++] = kAlphabet[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        const uint32_t v =
            (static_cast<uint8_t>(data[i])     << 16) |
            (static_cast<uint8_t>(data[i + 1]) <<  8);
        out[o++] = kAlphabet[(v >> 18) & 0x3F];
        out[o++] = kAlphabet[(v >> 12) & 0x3F];
        out[o++] = kAlphabet[(v >>  6) & 0x3F];
        out[o++] = '=';
    }
    return out;
}

// Standard base64 + URL-safe (RFC 4648 §5) decoder. Returns std::nullopt on
// any invalid input: out-of-alphabet characters, '=' in non-terminal positions,
// or wrong group length. Padding is optional (URL-safe variant often omits it).
//
// Test vectors (verified while writing — see tests/utils/base64_decode_test.cpp):
//   ""          -> ""
//   "TQ=="      -> "M"
//   "TWE="      -> "Ma"
//   "TWFu"      -> "Man"
//   "AP8Q"      -> "\x00\xff\x10"
//   "TWFu" (no pad)            -> "Man"
//   "aHR0cHM6Ly9leGFtcGxlLmNvbQ" (no pad) -> "https://example.com"
//   "_-A=" (URL-safe alphabet) -> "\xFF\xE0"
//   "@@@@" (invalid)           -> nullopt
inline std::optional<std::string> base64_decode(std::string_view in) {
    // 反查表:0..63 = 数据;-1 = 非法;-2 = 填充 '=';-3 = 跳过(空白)
    static constexpr int8_t kTable[256] = {
        // 0..15
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-3,-3,-1,-3,-3,-1,-1,
        // 16..31
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        // 32..47  (' ' '!' '"' '#' '$' '%' '&' '\'' '(' ')' '*' '+' ',' '-' '.' '/')
        -3,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63,
        // 48..63  ('0'..'9' ':' ';' '<' '=' '>' '?')
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        // 64..79  ('@' 'A'..'O')
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        // 80..95  ('P'..'Z' '[' '\\' ']' '^' '_')
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        // 96..111  ('`' 'a'..'o')
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        // 112..127  ('p'..'z' '{' '|' '}' '~')
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        // 128..255  全部非法
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::string out;
    out.reserve((in.size() / 4) * 3 + 3);

    uint32_t buf = 0;
    int buf_bits = 0;
    bool seen_pad = false;

    for (char c : in) {
        int8_t v = kTable[static_cast<uint8_t>(c)];
        if (v == -3) continue;             // 空白跳过
        if (v == -2) {                     // 填充 '='
            seen_pad = true;
            continue;
        }
        if (v == -1) return std::nullopt;  // 非法字符
        if (seen_pad) return std::nullopt; // 填充后又见数据
        buf = (buf << 6) | static_cast<uint32_t>(v);
        buf_bits += 6;
        if (buf_bits >= 8) {
            buf_bits -= 8;
            out.push_back(static_cast<char>((buf >> buf_bits) & 0xFF));
        }
    }

    // 长度合法性:base64 末尾 buf_bits 应为 0 / 2 / 4 之一
    // (对应 group 大小 4 / 2 / 3 个有效字符)
    if (buf_bits != 0 && buf_bits != 2 && buf_bits != 4) {
        return std::nullopt;
    }
    return out;
}

} // namespace acecode
