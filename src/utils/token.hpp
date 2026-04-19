#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/types.h>
#endif

namespace acecode {

// Fill `n` bytes from a cryptographically secure random source.
// Throws std::runtime_error on failure (rare; treat as fatal).
inline void secure_random_bytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    NTSTATUS s = ::BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                                   BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (s != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
#else
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("open(/dev/urandom) failed");
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, out + got, n - got);
        if (r < 0) {
            ::close(fd);
            throw std::runtime_error("read(/dev/urandom) failed");
        }
        got += static_cast<size_t>(r);
    }
    ::close(fd);
#endif
}

// Encode bytes as url-safe base64 without padding (RFC 4648 §5).
inline std::string base64url_no_pad(const uint8_t* data, size_t n) {
    static constexpr char alphabet[65] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >> 6) & 0x3F]);
        out.push_back(alphabet[v & 0x3F]);
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
    } else if (rem == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >> 6) & 0x3F]);
    }
    return out;
}

// Generate a 32-byte url-safe base64 token (43 chars), suitable for
// `~/.acecode/run/token`. Caller is responsible for restricting file
// permissions when persisting to disk.
inline std::string generate_auth_token() {
    uint8_t raw[32];
    secure_random_bytes(raw, sizeof(raw));
    return base64url_no_pad(raw, sizeof(raw));
}

} // namespace acecode
