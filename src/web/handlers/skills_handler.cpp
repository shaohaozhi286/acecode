#include "skills_handler.hpp"

#include "../../skills/skill_registry.hpp"
#include "../../utils/logger.hpp"

#include <algorithm>
#include <unordered_set>

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

} // namespace

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
