#include "skill_init.hpp"

#include "skill_registry.hpp"
#include "../config/config.hpp"
#include "../utils/encoding.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"

#include <filesystem>
#include <unordered_set>
#include <vector>

namespace acecode {

namespace {

namespace fs = std::filesystem;

std::string env_or_expanded_path(const char* name, const std::string& fallback) {
    std::string value = getenv_utf8(name);
    if (!value.empty()) return value;
    return expand_path(fallback);
}

void append_opencode_config_skill_roots(std::vector<fs::path>& roots,
                                        const fs::path& base) {
    if (base.empty()) return;
    roots.emplace_back(base / "skills");
    roots.emplace_back(base / "skill");
}

void append_opencode_project_skill_roots(std::vector<fs::path>& roots,
                                         const fs::path& dir) {
    append_opencode_config_skill_roots(roots, dir / ".opencode");
    roots.emplace_back(dir / ".agents" / "skills");
    roots.emplace_back(dir / ".claude" / "skills");
}

void append_opencode_global_skill_roots(std::vector<fs::path>& roots) {
    const fs::path xdg_config =
        path_from_utf8(env_or_expanded_path("XDG_CONFIG_HOME", "~/.config"));
    append_opencode_config_skill_roots(roots, xdg_config / "opencode");

    const fs::path home_opencode = path_from_utf8(expand_path("~/.opencode"));
    append_opencode_config_skill_roots(roots, home_opencode);

    const std::string custom_config = getenv_utf8("OPENCODE_CONFIG_DIR");
    if (!custom_config.empty()) {
        append_opencode_config_skill_roots(roots, path_from_utf8(custom_config));
    }

    roots.emplace_back(path_from_utf8(expand_path("~/.agents/skills")));
    roots.emplace_back(path_from_utf8(expand_path("~/.claude/skills")));

    const fs::path xdg_cache =
        path_from_utf8(env_or_expanded_path("XDG_CACHE_HOME", "~/.cache"));
    roots.emplace_back(xdg_cache / "opencode" / "skills");
}

} // namespace

std::vector<std::filesystem::path> project_skill_scan_roots(
    const AppConfig& config, const std::string& working_dir) {
    std::vector<std::filesystem::path> roots;

    for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
        roots.emplace_back(path_from_utf8(dir) / ".acecode" / "skills");
    }
    for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
        roots.emplace_back(path_from_utf8(dir) / ".agent" / "skills");
    }
    if (config.skills.reuse_opencode) {
        for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
            append_opencode_project_skill_roots(roots, path_from_utf8(dir));
        }
    }
    return roots;
}

std::vector<std::filesystem::path> global_skill_scan_roots(const AppConfig& config) {
    std::vector<std::filesystem::path> roots;

    roots.emplace_back(path_from_utf8(get_acecode_dir()) / "skills");
    roots.emplace_back(path_from_utf8(expand_path("~/.agent/skills")));

    if (config.skills.reuse_opencode) {
        append_opencode_global_skill_roots(roots);
    }

    for (const auto& raw : config.skills.external_dirs) {
        std::string expanded = expand_path(raw);
        if (!expanded.empty()) roots.emplace_back(path_from_utf8(expanded));
    }
    return roots;
}

void initialize_skill_registry(SkillRegistry& skill_registry,
                                 const AppConfig& config,
                                 const std::string& working_dir,
                                 const std::vector<std::filesystem::path>&
                                     prepended_roots) {
    std::error_code ec;

    std::vector<std::filesystem::path> roots = prepended_roots;
    auto project_roots = project_skill_scan_roots(config, working_dir);
    for (auto& root : project_roots) roots.emplace_back(std::move(root));

    // ~/.acecode/skills 是历史上自动创建的全局根,保持行为不变。
    std::filesystem::path default_acecode_skills_dir =
        path_from_utf8(get_acecode_dir()) / "skills";
    if (!std::filesystem::exists(default_acecode_skills_dir, ec)) {
        std::filesystem::create_directories(default_acecode_skills_dir, ec);
    }

    for (auto& root : global_skill_scan_roots(config)) {
        roots.emplace_back(std::move(root));
    }

    skill_registry.set_scan_roots(std::move(roots));
    skill_registry.set_disabled(std::unordered_set<std::string>(
        config.skills.disabled.begin(), config.skills.disabled.end()));
    if (config.skills.allowed) {
        skill_registry.set_allowed(std::unordered_set<std::string>(
            config.skills.allowed->begin(), config.skills.allowed->end()));
    } else {
        skill_registry.set_allowed(std::nullopt);
    }
    skill_registry.scan();
}

} // namespace acecode
