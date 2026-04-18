#include "skill_registry.hpp"

#include "frontmatter.hpp"
#include "skill_loader.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace acecode {

namespace {

const std::array<std::string, 3> kExcludedDirs = {".git", ".github", ".hub"};

bool is_excluded_segment(const fs::path& path) {
    for (const auto& part : path) {
        std::string s = part.string();
        for (const auto& ex : kExcludedDirs) {
            if (s == ex) return true;
        }
    }
    return false;
}

} // namespace

void SkillRegistry::set_scan_roots(std::vector<fs::path> roots) {
    std::lock_guard<std::mutex> lk(mu_);
    roots_ = std::move(roots);
}

void SkillRegistry::set_disabled(std::unordered_set<std::string> disabled) {
    std::lock_guard<std::mutex> lk(mu_);
    disabled_ = std::move(disabled);
}

void SkillRegistry::scan() {
    std::vector<fs::path> roots;
    std::unordered_set<std::string> disabled;
    {
        std::lock_guard<std::mutex> lk(mu_);
        roots = roots_;
        disabled = disabled_;
    }

    std::vector<SkillMetadata> found;
    std::unordered_set<std::string> seen_names;

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;

        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it->is_directory()) {
                std::string name = it->path().filename().string();
                for (const auto& ex : kExcludedDirs) {
                    if (name == ex) { it.disable_recursion_pending(); break; }
                }
                continue;
            }
            if (!it->is_regular_file()) continue;
            if (it->path().filename() != "SKILL.md") continue;
            if (is_excluded_segment(it->path())) continue;

            auto meta = load_skill_from_dir(it->path().parent_path(), root);
            if (!meta) continue;
            if (!skill_matches_platform(meta->platforms)) continue;
            if (disabled.count(meta->name)) continue;
            if (seen_names.count(meta->name)) continue;

            seen_names.insert(meta->name);
            found.push_back(std::move(*meta));
        }
    }

    std::sort(found.begin(), found.end(), [](const SkillMetadata& a, const SkillMetadata& b) {
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });

    {
        std::lock_guard<std::mutex> lk(mu_);
        skills_ = std::move(found);
    }
    LOG_INFO("[skills] Loaded " + std::to_string(skills_.size()) + " skills from " +
             std::to_string(roots.size()) + " root(s)");
}

std::vector<SkillMetadata> SkillRegistry::list(const std::string& category) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (category.empty()) return skills_;
    std::vector<SkillMetadata> filtered;
    for (const auto& s : skills_) {
        if (s.category == category) filtered.push_back(s);
    }
    return filtered;
}

std::optional<SkillMetadata> SkillRegistry::find(const std::string& name_or_key) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& s : skills_) {
        if (s.name == name_or_key || s.command_key == name_or_key) return s;
    }
    return std::nullopt;
}

std::string SkillRegistry::read_skill_body(const std::string& name) const {
    auto meta = find(name);
    if (!meta) return "";
    std::ifstream ifs(meta->skill_md_path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::ostringstream oss;
    oss << ifs.rdbuf();
    auto [fm, body] = parse_frontmatter(oss.str());
    (void)fm;
    return body;
}

std::vector<std::string> SkillRegistry::list_supporting_files(const std::string& name) const {
    auto meta = find(name);
    if (!meta) return {};
    std::vector<std::string> out;
    const std::array<std::string, 4> subdirs = {"references", "templates", "scripts", "assets"};
    for (const auto& sub : subdirs) {
        fs::path dir = meta->skill_dir / sub;
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
        std::vector<std::string> bucket;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file()) continue;
            std::error_code rec;
            auto rel = fs::relative(it->path(), meta->skill_dir, rec);
            if (rec) continue;
            bucket.push_back(rel.generic_string());
        }
        std::sort(bucket.begin(), bucket.end());
        for (auto& s : bucket) out.push_back(std::move(s));
    }
    return out;
}

std::optional<fs::path> SkillRegistry::resolve_skill_file(
    const std::string& name, const std::string& relative_path) const {
    auto meta = find(name);
    if (!meta) return std::nullopt;
    if (relative_path.empty()) return std::nullopt;

    // Reject explicit traversal tokens before hitting the filesystem.
    if (relative_path.find("..") != std::string::npos) return std::nullopt;

    fs::path target = meta->skill_dir / relative_path;
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(target, ec);
    if (ec) resolved = target;
    fs::path root = fs::weakly_canonical(meta->skill_dir, ec);
    if (ec) root = meta->skill_dir;

    // Ensure the resolved path stays under the skill directory.
    auto root_str = root.generic_string();
    auto res_str = resolved.generic_string();
    if (res_str.size() < root_str.size()) return std::nullopt;
    if (res_str.compare(0, root_str.size(), root_str) != 0) return std::nullopt;
    if (res_str.size() > root_str.size() && res_str[root_str.size()] != '/') {
        return std::nullopt;
    }
    return resolved;
}

} // namespace acecode
