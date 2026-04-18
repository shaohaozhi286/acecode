#pragma once

#include <cstddef>
#include <cstdint>
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

} // namespace acecode
