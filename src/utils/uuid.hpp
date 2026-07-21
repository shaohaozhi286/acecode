#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstdint>

namespace acecode {

// Generate a unique identifier string (UUID v4-like format)
inline std::string generate_uuid() {
    static thread_local std::mt19937 rng(
        static_cast<unsigned int>(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    std::uniform_int_distribution<int> dist(0, 15);
    std::uniform_int_distribution<int> dist2(8, 11); // variant bits

    std::ostringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; ++i) ss << dist(rng);
    ss << '-';
    for (int i = 0; i < 4; ++i) ss << dist(rng);
    ss << "-4"; // version 4
    for (int i = 0; i < 3; ++i) ss << dist(rng);
    ss << '-';
    ss << dist2(rng); // variant
    for (int i = 0; i < 3; ++i) ss << dist(rng);
    ss << '-';
    for (int i = 0; i < 12; ++i) ss << dist(rng);

    return ss.str();
}

// Generate a time-ordered UUID v7 identifier. Compact context-window ids use
// this form to match Codex's persisted compaction metadata.
inline std::string generate_uuid_v7() {
    static thread_local std::mt19937_64 rng(
        static_cast<std::uint64_t>(std::random_device{}()) ^
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto timestamp_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) &
        0x0000FFFFFFFFFFFFULL;
    const std::uint64_t rand_a = rng() & 0x0FFFULL;
    const std::uint64_t rand_b = rng() & 0x3FFFFFFFFFFFFFFFULL;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(8) << static_cast<std::uint32_t>(timestamp_ms >> 16)
       << '-'
       << std::setw(4) << static_cast<std::uint16_t>(timestamp_ms)
       << '-'
       << std::setw(4) << static_cast<std::uint16_t>(0x7000ULL | rand_a)
       << '-'
       << std::setw(4) << static_cast<std::uint16_t>(
              0x8000ULL | ((rand_b >> 48) & 0x3FFFULL))
       << '-'
       << std::setw(12) << (rand_b & 0x0000FFFFFFFFFFFFULL);
    return ss.str();
}

// Get current ISO 8601 timestamp
inline std::string iso_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm buf;
#ifdef _WIN32
    gmtime_s(&buf, &t);
#else
    gmtime_r(&t, &buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&buf, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

} // namespace acecode
