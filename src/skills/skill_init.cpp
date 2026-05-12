#include "skill_init.hpp"

#include "skill_registry.hpp"
#include "../config/config.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"

#include <filesystem>
#include <unordered_set>
#include <vector>

namespace acecode {

void initialize_skill_registry(SkillRegistry& skill_registry,
                                 const AppConfig& config,
                                 const std::string& working_dir) {
    std::vector<std::filesystem::path> roots;
    std::error_code ec;

    for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
        roots.emplace_back(path_from_utf8(dir) / ".acecode" / "skills");
    }
    for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
        roots.emplace_back(path_from_utf8(dir) / ".agent" / "skills");
    }

    std::filesystem::path default_acecode_skills_dir =
        path_from_utf8(get_acecode_dir()) / "skills";
    if (!std::filesystem::exists(default_acecode_skills_dir, ec)) {
        std::filesystem::create_directories(default_acecode_skills_dir, ec);
    }
    roots.emplace_back(default_acecode_skills_dir);

    std::string default_agent_skills_dir = expand_path("~/.agent/skills");
    roots.emplace_back(path_from_utf8(default_agent_skills_dir));

    for (const auto& raw : config.skills.external_dirs) {
        std::string expanded = expand_path(raw);
        if (!expanded.empty()) roots.emplace_back(path_from_utf8(expanded));
    }

    skill_registry.set_scan_roots(std::move(roots));
    skill_registry.set_disabled(std::unordered_set<std::string>(
        config.skills.disabled.begin(), config.skills.disabled.end()));
    skill_registry.scan();
}

} // namespace acecode
