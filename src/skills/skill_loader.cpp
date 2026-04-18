#include "skill_loader.hpp"

#include "frontmatter.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr size_t FRONTMATTER_READ_BUDGET = 8 * 1024; // first 8KB is plenty

std::string read_frontmatter_chunk(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::string buf(FRONTMATTER_READ_BUDGET, '\0');
    ifs.read(buf.data(), static_cast<std::streamsize>(FRONTMATTER_READ_BUDGET));
    buf.resize(static_cast<size_t>(ifs.gcount()));
    return buf;
}

std::string truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n > 3 ? n - 3 : n) + "...";
}

std::string first_non_empty_body_line(const std::string& body) {
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t;
        t.reserve(line.size());
        size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        while (i < line.size()) t.push_back(line[i++]);
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        if (t.empty()) continue;
        if (t.front() == '#') continue;
        return t;
    }
    return "";
}

std::string derive_category(const fs::path& skill_dir, const fs::path& scan_root) {
    std::error_code ec;
    auto rel = fs::relative(skill_dir, scan_root, ec);
    if (ec) return "";
    auto it = rel.begin();
    if (it == rel.end()) return "";
    // rel = "<category>/<name>" (at minimum two components) → first segment
    // is the category. A flat "<name>" layout has no category.
    auto first = *it;
    ++it;
    if (it == rel.end()) return "";
    return first.string();
}

} // namespace

std::string current_platform_identifier() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

bool skill_matches_platform(const std::vector<std::string>& platforms) {
    if (platforms.empty()) return true;
    std::string current = current_platform_identifier();
    for (const auto& p : platforms) {
        std::string lower;
        lower.reserve(p.size());
        for (char c : p) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower == current) return true;
    }
    return false;
}

std::string normalize_skill_command_key(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc == ' ' || uc == '_') {
            out.push_back('-');
        } else if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        } else if (c == '-') {
            out.push_back('-');
        }
        // else: drop silently (matches hermes behaviour)
    }
    // Collapse multiple hyphens and trim.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool last_hyphen = false;
    for (char c : out) {
        if (c == '-') {
            if (last_hyphen) continue;
            last_hyphen = true;
        } else {
            last_hyphen = false;
        }
        collapsed.push_back(c);
    }
    while (!collapsed.empty() && collapsed.front() == '-') collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back() == '-')  collapsed.pop_back();
    return collapsed;
}

std::optional<SkillMetadata> load_skill_from_dir(const fs::path& dir,
                                                 const fs::path& scan_root) {
    fs::path skill_md = dir / "SKILL.md";
    std::error_code ec;
    if (!fs::exists(skill_md, ec) || !fs::is_regular_file(skill_md, ec)) {
        return std::nullopt;
    }

    std::string chunk = read_frontmatter_chunk(skill_md);
    if (chunk.empty()) {
        LOG_WARN("[skills] Empty or unreadable SKILL.md: " + skill_md.string());
        return std::nullopt;
    }

    auto [fm, body] = parse_frontmatter(chunk);

    SkillMetadata meta;
    meta.skill_md_path = skill_md;
    meta.skill_dir = dir;

    std::string fm_name = get_string(fm, "name");
    if (fm_name.empty()) {
        meta.name = dir.filename().string();
        LOG_WARN("[skills] " + skill_md.string() + " missing frontmatter 'name'; using dir name '" + meta.name + "'");
    } else {
        meta.name = fm_name;
    }

    meta.command_key = normalize_skill_command_key(meta.name);
    if (meta.command_key.empty()) {
        LOG_WARN("[skills] Skill name '" + meta.name + "' has no usable command slug; skipping " + skill_md.string());
        return std::nullopt;
    }

    std::string desc = get_string(fm, "description");
    if (desc.empty()) desc = first_non_empty_body_line(body);
    meta.description = truncate(desc, 1024);

    meta.category = derive_category(dir, scan_root);
    meta.platforms = get_list(fm, "platforms");

    // tags live under metadata.hermes.tags (hermes convention) OR metadata.tags
    if (const FrontmatterValue* tv = get_nested(fm, {"metadata", "hermes", "tags"})) {
        if (tv->is_list()) meta.tags = tv->list_value;
    }
    if (meta.tags.empty()) {
        if (const FrontmatterValue* tv = get_nested(fm, {"metadata", "tags"})) {
            if (tv->is_list()) meta.tags = tv->list_value;
        }
    }

    return meta;
}

} // namespace acecode
