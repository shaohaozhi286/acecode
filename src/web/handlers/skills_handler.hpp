#pragma once

// /api/skills/:name 启停 + /api/skills/:name/body 端点的纯逻辑(可单测)。
// HTTP 路由注册在 server.cpp。
//
// 启停语义:
//   - enabled=true:从 cfg.skills.disabled 移除该 name(若存在)
//   - enabled=false:加入 cfg.skills.disabled(去重)
//   - 校验 name 必须是"已知的"(在 registry.find() 找到 OR 在 disabled 列表里)
//     —— 否则 404,避免随便发个名字就写入 disabled 数组
//   - 操作完调用 save_config + 同步 registry.set_disabled + reload

#include "../../config/config.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace acecode {
class SkillRegistry;
}

namespace acecode::web {

struct SkillToggleResult {
    bool           ok = false;
    int            http_status = 200;
    nlohmann::json body;
};

// 切换 skill 启停。config_path 空 = 用 save_config 默认路径(~/.acecode/config.json)。
SkillToggleResult set_skill_enabled(const std::string& name,
                                       bool enabled,
                                       AppConfig& cfg,
                                       SkillRegistry& registry,
                                       const std::string& config_path);

// 读 SKILL.md 全文。返回 nullopt = 该 name 未注册(404)。
std::optional<std::string> get_skill_body(const std::string& name,
                                            const SkillRegistry& registry);

} // namespace acecode::web
