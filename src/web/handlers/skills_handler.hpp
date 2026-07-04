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

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode {
class SkillRegistry;
}

namespace acecode::web {

struct SkillToggleResult {
    bool           ok = false;
    int            http_status = 200;
    nlohmann::json body;
};

struct SkillRootSelection {
    std::filesystem::path path;
    std::string           source;
    std::filesystem::path global_path; // 用户全局 skills 根(~/.acecode/skills)
};

SkillRootSelection select_skill_root(const std::filesystem::path& workspace_cwd,
                                     const std::filesystem::path& global_acecode_skills_root,
                                     bool create_missing_roots);

SkillRootSelection resolve_skill_root_for_cwd(const std::string& workspace_cwd_utf8);

// 切换 skill 启停。config_path 空 = 用 save_config 默认路径(~/.acecode/config.json)。
// lookup_registry:可选的"已知性"校验 registry(如某个 workspace 的临时扫描
// 结果)— daemon 全局 registry 只扫 daemon cwd 的项目链,其它 workspace 的
// skill 在其中 find 不到;不传时用 registry 本身校验。disabled 同步始终打在
// registry(daemon 全局)上。
SkillToggleResult set_skill_enabled(const std::string& name,
                                       bool enabled,
                                       AppConfig& cfg,
                                       SkillRegistry& registry,
                                       const std::string& config_path,
                                       const SkillRegistry* lookup_registry = nullptr);

// 读 SKILL.md 全文。返回 nullopt = 该 name 未注册(404)。
std::optional<std::string> get_skill_body(const std::string& name,
                                            const SkillRegistry& registry);

// 构建 GET /api/skills 的响应数组:对给定扫描根做一次全量扫描(不过滤
// disabled),每条带
//   - enabled:name 不在 disabled 中
//   - source:"project"(命中 project_roots)/ "global"(其余根)
// disabled 中磁盘上已不存在的名字(幽灵条目)仍以 enabled=false、source=""
// 列出,让用户能把它从禁用列表里放出来。
nlohmann::json build_skills_payload_with_roots(
    const std::vector<std::filesystem::path>& project_roots,
    const std::vector<std::filesystem::path>& global_roots,
    const std::vector<std::string>& disabled);

// 便捷封装:扫描根取自 skill_init 的 project/global 根构成(与
// initialize_skill_registry 一致),disabled 取自 cfg.skills.disabled。
nlohmann::json build_skills_payload(const AppConfig& cfg,
                                    const std::string& workspace_cwd_utf8);

} // namespace acecode::web
