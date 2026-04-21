#include "memory_frontmatter.hpp"

#include "../skills/frontmatter.hpp"
#include "../utils/logger.hpp"

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string read_file_to_string(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// Escape a scalar string for the rendered frontmatter. Quotes are minimal:
// always emit double-quoted values so round-tripping strings with colons or
// leading/trailing whitespace stays unambiguous.
std::string render_scalar(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

std::optional<MemoryEntry> parse_memory_entry_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }

    std::string content = read_file_to_string(path);
    if (content.empty()) {
        LOG_WARN("[memory] entry file is empty or unreadable: " + path.string());
        return std::nullopt;
    }

    auto [fm, body] = parse_frontmatter(content);
    if (fm.empty()) {
        LOG_WARN("[memory] entry missing frontmatter: " + path.string());
        return std::nullopt;
    }

    std::string desc = get_string(fm, "description", "");
    std::string type_s = get_string(fm, "type", "");
    if (desc.empty() || type_s.empty()) {
        LOG_WARN("[memory] entry missing required field (description/type): " +
                 path.string());
        return std::nullopt;
    }

    auto parsed_type = parse_memory_type(type_s);
    if (!parsed_type.has_value()) {
        LOG_WARN("[memory] entry has invalid type '" + type_s +
                 "' (allowed: user|feedback|project|reference): " + path.string());
        return std::nullopt;
    }

    MemoryEntry entry;
    // On-disk name wins: derive from the file stem so callers never confuse
    // frontmatter `name` with the identifier used for lookup.
    entry.name = path.stem().string();
    entry.description = desc;
    entry.type = *parsed_type;
    entry.path = path;
    entry.body = body;
    return entry;
}

std::string render_memory_entry(const MemoryEntry& entry) {
    std::ostringstream oss;
    oss << "---\n"
        << "name: "        << render_scalar(entry.name)        << "\n"
        << "description: " << render_scalar(entry.description) << "\n"
        << "type: "        << memory_type_to_string(entry.type) << "\n"
        << "---\n\n"
        << entry.body;
    // Ensure a trailing newline so tools like `git diff` stay sane.
    if (entry.body.empty() || entry.body.back() != '\n') {
        oss << "\n";
    }
    return oss.str();
}

} // namespace acecode
