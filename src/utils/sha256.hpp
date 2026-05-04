#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace acecode {

class Sha256 {
public:
    Sha256() { reset(); }

    void update(const unsigned char* data, std::size_t len) {
        bit_len_ += static_cast<std::uint64_t>(len) * 8u;
        while (len > 0) {
            const std::size_t room = 64 - buffer_len_;
            const std::size_t take = (len < room) ? len : room;
            std::memcpy(buffer_.data() + buffer_len_, data, take);
            buffer_len_ += take;
            data += take;
            len -= take;
            if (buffer_len_ == 64) {
                transform(buffer_.data());
                buffer_len_ = 0;
            }
        }
    }

    void update(const std::string& s) {
        update(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    }

    std::string final_hex() {
        std::array<unsigned char, 64> tail = buffer_;
        std::size_t len = buffer_len_;
        tail[len++] = 0x80;

        if (len > 56) {
            while (len < 64) tail[len++] = 0;
            transform(tail.data());
            tail.fill(0);
            len = 0;
        }

        while (len < 56) tail[len++] = 0;
        for (int i = 7; i >= 0; --i) {
            tail[len++] = static_cast<unsigned char>((bit_len_ >> (i * 8)) & 0xffu);
        }
        transform(tail.data());

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (std::uint32_t h : state_) {
            oss << std::setw(8) << h;
        }
        return oss.str();
    }

private:
    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffer_len_ = 0;
    std::uint64_t bit_len_ = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    static std::uint32_t choose(std::uint32_t e, std::uint32_t f, std::uint32_t g) {
        return (e & f) ^ ((~e) & g);
    }

    static std::uint32_t majority(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        return (a & b) ^ (a & c) ^ (b & c);
    }

    void reset() {
        state_ = {
            0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
        };
        buffer_.fill(0);
        buffer_len_ = 0;
        bit_len_ = 0;
    }

    void transform(const unsigned char* chunk) {
        static constexpr std::array<std::uint32_t, 64> k = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };

        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(chunk[i * 4 + 0]) << 24) |
                   (static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8) |
                   (static_cast<std::uint32_t>(chunk[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t temp1 = h + s1 + choose(e, f, g) + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t temp2 = s0 + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }
};

inline std::string sha256_hex(const std::string& input) {
    Sha256 sha;
    sha.update(input);
    return sha.final_hex();
}

inline std::string sha256_file_hex(const std::string& path, std::string* error = nullptr) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        if (error) *error = "failed to open file for SHA-256: " + path;
        return "";
    }
    Sha256 sha;
    std::array<char, 64 * 1024> buf{};
    while (ifs) {
        ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = ifs.gcount();
        if (n > 0) {
            sha.update(reinterpret_cast<const unsigned char*>(buf.data()),
                       static_cast<std::size_t>(n));
        }
    }
    if (ifs.bad()) {
        if (error) *error = "failed to read file for SHA-256: " + path;
        return "";
    }
    return sha.final_hex();
}

} // namespace acecode
