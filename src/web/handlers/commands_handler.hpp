#pragma once

// /api/commands 端点的纯逻辑(可单测)。HTTP 路由注册在 server.cpp。
//
// 该端点只读,只暴露 web 端有意义的命令清单 — 当前白名单 = init + compact
// + goal + plan + opencode markdown commands + 所有已启用的 skill。其它
// builtin(model/resume/rewind/clear 等)与 web 多会话语义冲突或已有专门 UI,
// 故不暴露。
//
// 选中后**不**触发后端执行(选中只插入 textarea)。提交时,前端会把 init /
// compact/goal/plan 路由到 `POST /api/sessions/:id/commands`,opencode commands
// 与 skills 继续走普通消息路径,由 daemon sendInput expansion 展开。详见
// openspec/changes/add-webui-slash-commands 与
// openspec/changes/support-opencode-command-folders。

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace acecode {
class SkillRegistry;
struct AppConfig;
}

namespace acecode::web {

// 拼装 GET /api/commands 响应:
//   {builtins: [{name,description}], commands: [{name,description}], skills: [...]}
// builtins 顺序固定 init→compact→goal→plan;commands/skills 按 name 字典序。
//
// workspace_cwd:可选。若非空 + cfg 非空,会按该 workspace cwd 走一次 skill
// 扫描根计算(.acecode/skills、.agent/skills 项目链 + 全局根),与 global_skills
// 合并(按 name 去重,workspace local 优先 — first-wins 与 SkillRegistry 一致)。
//
// Desktop 多 workspace 场景下 daemon 启动 cwd 固定,global_skills 只能看到
// daemon 自己 cwd 链的项目 skills;前端切到 workspace X 时传 X 的 cwd 进来,
// X 项目里的 .agent/skills 才会被列出。
nlohmann::json build_commands_payload(const SkillRegistry& global_skills,
                                        const std::optional<std::string>& workspace_cwd = std::nullopt,
                                        const AppConfig* cfg = nullptr);

} // namespace acecode::web
