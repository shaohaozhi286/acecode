#include "memory_paths.hpp"

#include "../config/config.hpp"

#include <algorithm>

namespace fs = std::filesystem;

namespace acecode {

fs::path get_memory_dir() {
    return fs::path(get_acecode_dir()) / "memory";
}

fs::path get_memory_index_path() {
    return get_memory_dir() / "MEMORY.md";
}

std::string validate_memory_name(const std::string& name) {
    if (name.empty()) return "memory name is empty";
    if (name.size() > 64) return "memory name exceeds 64 bytes: " + name;
    for (unsigned char c : name) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-';
        if (!ok) {
            return std::string("memory name has invalid character '") +
                   static_cast<char>(c) + "'; allowed: [A-Za-z0-9_-]";
        }
    }
    // Reserve MEMORY as the index filename. Case-insensitive to avoid a
    // Windows user creating a memory called "memory" that collides with
    // MEMORY.md on a case-insensitive filesystem.
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower == "memory") return "memory name 'MEMORY' is reserved for the index file";
    return "";
}

fs::path resolve_memory_entry_path(const std::string& name) {
    if (!validate_memory_name(name).empty()) return {};
    return get_memory_dir() / (name + ".md");
}

bool is_within_memory_dir(const fs::path& path) {
    std::error_code ec;
    fs::path canonical_target = fs::weakly_canonical(path, ec);
    if (ec) canonical_target = path;

    fs::path canonical_memory = fs::weakly_canonical(get_memory_dir(), ec);
    if (ec) canonical_memory = get_memory_dir();

    // Compare as generic (forward-slash) strings case-insensitively on Windows,
    // case-sensitively elsewhere. A simple lexical prefix check is enough here
    // because weakly_canonical has already resolved .. and symlinks.
    std::string target_s = canonical_target.generic_string();
    std::string memory_s = canonical_memory.generic_string();
    while (!memory_s.empty() && memory_s.back() == '/') memory_s.pop_back();

#ifdef _WIN32
    auto to_lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    target_s = to_lower(target_s);
    memory_s = to_lower(memory_s);
#endif

    if (target_s.size() <= memory_s.size()) return false;
    if (target_s.compare(0, memory_s.size(), memory_s) != 0) return false;
    // Guard against prefix matches like `memory_foo` vs `memory`.
    return target_s[memory_s.size()] == '/';
}

} // namespace acecode
