#include "skills_handler.hpp"

#include "../../skills/skill_init.hpp"
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

void create_directory_if_requested(const fs::path& path, bool requested) {
    if (!requested || path.empty()) return;

    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        LOG_WARN("failed to create skill directory " + path_to_utf8(path) + ": " + ec.message());
    }
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
                                     bool create_missing_roots) {
    const fs::path global_norm = normalized_path(global_acecode_skills_root);

    if (!workspace_cwd.empty()) {
        const auto acecode_project_skills = workspace_cwd / ".acecode" / "skills";
        if (existing_directory(acecode_project_skills)) {
            return {normalized_path(acecode_project_skills), "project_acecode", global_norm};
        }

        const auto agent_project_skills = workspace_cwd / ".agent" / "skills";
        if (existing_directory(agent_project_skills)) {
            return {normalized_path(agent_project_skills), "project_agent", global_norm};
        }

        if (create_missing_roots && existing_directory(workspace_cwd)) {
            create_directory_if_requested(acecode_project_skills, true);
            if (existing_directory(acecode_project_skills)) {
                return {normalized_path(acecode_project_skills), "project_acecode", global_norm};
            }
        }
    }

    create_directory_if_requested(global_acecode_skills_root, create_missing_roots);
    return {global_norm, "global_acecode", global_norm};
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
                                       const std::string& config_path,
                                       const SkillRegistry* lookup_registry) {
    SkillToggleResult out;

    // 校验 name 是 "已知的":要么当前在 lookup registry(enabled),要么在
    // disabled list。lookup 缺省 = daemon 全局 registry。
    const SkillRegistry& lookup = lookup_registry ? *lookup_registry : registry;
    bool currently_enabled  = lookup.find(name).has_value();
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

nlohmann::json build_skills_payload_with_roots(
    const std::vector<fs::path>& project_roots,
    const std::vector<fs::path>& global_roots,
    const std::vector<std::string>& disabled_list) {
    // 扫描时 disabled 置空 —— 禁用中的 skill 也保留完整元数据(描述/来源),
    // 而不是像旧实现那样只剩一个名字。
    std::unordered_set<std::string> project_root_keys;
    for (const auto& root : project_roots) {
        project_root_keys.insert(path_to_utf8(root.lexically_normal()));
    }

    std::vector<fs::path> roots = project_roots;
    roots.insert(roots.end(), global_roots.begin(), global_roots.end());

    SkillRegistry registry;
    registry.set_scan_roots(std::move(roots));
    registry.set_disabled({});
    registry.scan();

    const std::unordered_set<std::string> disabled(
        disabled_list.begin(), disabled_list.end());

    nlohmann::json arr = nlohmann::json::array();
    std::unordered_set<std::string> listed;
    for (const auto& s : registry.list()) {
        nlohmann::json o;
        o["name"]        = s.name;
        o["command_key"] = s.command_key;
        o["description"] = s.description;
        o["category"]    = s.category;
        o["enabled"]     = disabled.count(s.name) == 0;
        o["source"]      = project_root_keys.count(
                               path_to_utf8(s.scan_root.lexically_normal()))
                               ? "project" : "global";
        listed.insert(s.name);
        arr.push_back(std::move(o));
    }

    // 幽灵条目:disabled 里残留、但磁盘上已经找不到的名字。仍然列出,
    // 让用户能从禁用列表里清掉它。
    for (const auto& name : disabled_list) {
        if (listed.count(name)) continue;
        nlohmann::json o;
        o["name"]        = name;
        o["command_key"] = name;
        o["description"] = "";
        o["category"]    = "";
        o["enabled"]     = false;
        o["source"]      = "";
        arr.push_back(std::move(o));
    }
    return arr;
}

nlohmann::json build_skills_payload(const AppConfig& cfg,
                                    const std::string& workspace_cwd_utf8) {
    return build_skills_payload_with_roots(
        project_skill_scan_roots(cfg, workspace_cwd_utf8),
        global_skill_scan_roots(cfg),
        cfg.skills.disabled);
}

} // namespace acecode::web
