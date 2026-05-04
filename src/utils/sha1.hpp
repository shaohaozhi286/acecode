#pragma once

// 自包含的 SHA-1 实现:用作 web 协议里给 assistant/tool/system 消息算稳定 ID
// 的 hash(user 消息走持久 UUID,这套不参与)。SHA-1 已不适合密码学场景,但
// 用作消息 ID 的命名空间内 collision-resistant tag 完全够用,且跨平台输出稳定。
//
// 选 SHA-1 而不是 FNV-1a / std::hash:
//   - FNV-1a 64-bit 在百万级消息里碰撞概率有意义,SHA-1 截断到 40 hex 不会
//   - std::hash 没有跨平台稳定性保证
//   - cwd_hash.cpp 已经用 FNV-1a 但那是 path-level、唯一性弱的场景
//
// 实现是经典的 RFC 3174 算法,无外部依赖,header-only inline。

#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <string>

namespace acecode {

namespace sha1_detail {

inline std::uint32_t rotl(std::uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

inline void process_block(const std::uint8_t* block, std::uint32_t state[5]) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] =  (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24)
              | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
              | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
              |  static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;            k = 0xCA62C1D6u; }
        std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

} // namespace sha1_detail

// 计算输入字符串的 SHA-1,返回 40 字符小写 hex。
inline std::string sha1_hex(const std::string& input) {
    std::uint32_t state[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
    };
    const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t len = input.size();

    // 整 64-byte 块
    std::size_t full_blocks = len / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        sha1_detail::process_block(data + i * 64, state);
    }

    // 余下 + padding
    std::array<std::uint8_t, 128> tail{};
    std::size_t rem = len - full_blocks * 64;
    if (rem > 0) std::memcpy(tail.data(), data + full_blocks * 64, rem);
    tail[rem] = 0x80;
    std::size_t tail_blocks = (rem < 56) ? 1 : 2;
    std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8;
    std::size_t len_off = tail_blocks * 64 - 8;
    for (int i = 0; i < 8; ++i) {
        tail[len_off + i] = static_cast<std::uint8_t>((bit_len >> ((7 - i) * 8)) & 0xFF);
    }
    for (std::size_t i = 0; i < tail_blocks; ++i) {
        sha1_detail::process_block(tail.data() + i * 64, state);
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 5; ++i) {
        oss << std::setw(8) << state[i];
    }
    return oss.str();
}

} // namespace acecode
