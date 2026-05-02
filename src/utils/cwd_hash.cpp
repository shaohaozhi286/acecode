#include "cwd_hash.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace acecode {

namespace {

// FNV-1a 64-bit。无外部依赖,与 src/session/session_storage.cpp 历史实现 byte-byte 等价。
std::uint64_t fnv1a_64(const std::string& data) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

std::string compute_cwd_hash(const std::string& cwd) {
    // 1. 反斜杠 → 正斜杠(跨平台一致)
    std::string normalized = cwd;
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }
    // 2. 全部小写(Windows / macOS 大小写不敏感的兜底;Linux 严格大小写但路径用户自己掌控)
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 3. 去尾斜杠("foo/" 与 "foo" 算同一目录)
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }

    std::uint64_t h = fnv1a_64(normalized);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << h;
    return oss.str();
}

} // namespace acecode
