#include "skills_handler.hpp"

#include "../../skills/skill_registry.hpp"
#include "../../utils/utf8_path.hpp"
#include "../../utils/logger.hpp"

#include <algorithm>
#include <system_error>
#include <unordered_set>

namespace fs = std::filesystem;

namespace acecode::web {

namespace {

bool is_in_disabled(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

void apply_registry_disabled_set(SkillRegistry& registry,
                                    const std::vector<std::string>& disabled) {
    std::unordered_set<std::string> ds(disabled.begin(), disabled.end());
    registry.set_disabled(std::move(ds));
    registry.reload();
}

bool existing_directory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

fs::path normalized_path(const fs::path& path) {
    std::error_code ec;
    auto resolved = fs::weakly_canonical(path, ec);
    if (!ec && !resolved.empty()) return resolved;
    return path.lexically_normal();
}

} // namespace

SkillRootSelection select_skill_root(const fs::path& workspace_cwd,
                                     const fs::path& global_acecode_skills_root,
                                     bool create_global_fallback) {
    if (!workspace_cwd.empty()) {
        const auto acecode_project_skills = workspace_cwd / ".acecode" / "skills";
        if (existing_directory(acecode_project_skills)) {
            return {normalized_path(acecode_project_skills), "project_acecode"};
        }

        const auto agent_project_skills = workspace_cwd / ".agent" / "skills";
        if (existing_directory(agent_project_skills)) {
            return {normalized_path(agent_project_skills), "project_agent"};
        }
    }

    if (create_global_fallback) {
        std::error_code ec;
        fs::create_directories(global_acecode_skills_root, ec);
    }
    return {normalized_path(global_acecode_skills_root), "global_acecode"};
}

SkillRootSelection resolve_skill_root_for_cwd(const std::string& workspace_cwd_utf8) {
    fs::path cwd;
    if (!workspace_cwd_utf8.empty()) cwd = acecode::path_from_utf8(workspace_cwd_utf8);
    const auto global_root = acecode::path_from_utf8(get_acecode_dir()) / "skills";
    return select_skill_root(cwd, global_root, true);
}

SkillToggleResult set_skill_enabled(const std::string& name,
                                       bool enabled,
                                       AppConfig& cfg,
                                       SkillRegistry& registry,
                                       const std::string& config_path) {
    SkillToggleResult out;

    // 校验 name 是 "已知的":要么当前在 registry(enabled),要么在 disabled list。
    bool currently_enabled  = registry.find(name).has_value();
    bool currently_disabled = is_in_disabled(cfg.skills.disabled, name);
    if (!currently_enabled && !currently_disabled) {
        out.ok = false;
        out.http_status = 404;
        out.body = nlohmann::json{{"error", "skill not found"}};
        return out;
    }

    auto& list = cfg.skills.disabled;
    if (enabled) {
        // 移除(若存在)
        list.erase(std::remove(list.begin(), list.end(), name), list.end());
    } else {
        // 加入(若未存在)
        if (!currently_disabled) list.push_back(name);
    }

    if (config_path.empty()) save_config(cfg);
    else                     save_config(cfg, config_path);

    apply_registry_disabled_set(registry, list);

    out.ok = true;
    out.http_status = 200;
    out.body = nlohmann::json{
        {"name",    name},
        {"enabled", enabled},
    };
    return out;
}

std::optional<std::string> get_skill_body(const std::string& name,
                                            const SkillRegistry& registry) {
    if (!registry.find(name).has_value()) return std::nullopt;
    std::string body = registry.read_skill_body(name);
    return body;
}

} // namespace acecode::web
