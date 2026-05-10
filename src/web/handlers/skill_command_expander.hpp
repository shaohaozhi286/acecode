#pragma once

// Daemon 端 skill 命令展开器 — 把 `/<skill-name> [args]` 形式的 user message
// rewrite 成轻量调用提示(skill 名 + description + skill_view 提示 + args),
// LLM 按需用 skill_view tool 加载完整 SKILL.md。**不**注入 SKILL.md body,
// 否则同 session 多次调用同名 skill 会让 context 成倍膨胀。
//
// 路径:`POST /api/sessions/:id/messages` 的 lambda 在调 `send_input()` 之前
// 调用 `try_expand_skill_command(text, registry)`,命中时用返回的 expanded
// text 替换原 text。`/init` 和 `/compact` 不应到达这里:Desktop/Web 提交
// 时会改走 `POST /api/sessions/:id/commands`。前端的 SlashDropdown / chip
// 行为保持不变。
//
// 设计要点见 openspec/changes/expand-webui-skill-commands/design.md。
//
// 复用 `src/skills/skill_activation.cpp::build_skill_invocation_hint`(TUI 已对齐),
// LLM 看到的内容跨 TUI/web 一致。SKILL.md body 在 LLM 第一次调 skill_view 时
// 才进入 context history,后续重复 `/skill` 调用不再注入。

#include <string>

namespace acecode {
class SkillRegistry;
}

namespace acecode::web {

struct SkillExpansionResult {
    bool        expanded = false;   // true = text 已经被替换为 activation 字符串
    std::string text;                // expanded=true 时是新 text;false 时是原文(透传)
    std::string skill_name;          // 命中的 skill 名(空 = 没命中);用于日志
};

// 纯函数:解析 original_text 首段,若是 registry 中已知 skill,展开为完整
// activation message;否则透传原文。不抛异常。
//   - text 为空 / 首字符非 `/` → expanded=false, text=original_text
//   - 首段空(只有 `/`) → expanded=false
//   - 首段不是已知 skill 名 → expanded=false, skill_name 填上(便于日志)
//   - 命中 → expanded=true, text=build_activation_message(...) 的输出
//
// `registry` 应该是按 session 所属 workspace cwd 临时构造的(见 design D2)。
SkillExpansionResult try_expand_skill_command(const std::string& original_text,
                                                const SkillRegistry& registry);

} // namespace acecode::web
