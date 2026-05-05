#pragma once

#include "skill_metadata.hpp"

#include <string>

namespace acecode {

// 用户敲 `/<skill-name> [args]` 时注入到对话的轻量调用提示。
//
// 设计要点(openspec/changes/expand-webui-skill-commands):
//   - **不**注入 SKILL.md body 或 supporting_files 列表 — 否则同 session 多次
//     调用同名 skill 会让上下文成倍膨胀。
//   - 只告诉 LLM:用户调了哪个 skill、传了什么 args、可以用 skill_view tool
//     按需加载完整 SKILL.md。LLM 第一次需要时主动 skill_view 一次,后续重复
//     调用不再注入 body(SKILL.md 已经在 context history 里)。
//   - 与 src/tool/skill_view_tool.cpp / skills_tool.cpp 中 `skills_list` /
//     `skill_view` 的 description("Returns only minimal metadata — use
//     skill_view to load full SKILL.md content") 对齐。
//
// 输出格式:
//
//   [SYSTEM: User invoked /<name> skill]
//
//   Description: <description>
//   Use skill_view(name="<name>") to load the full SKILL.md if you need details.
//
//   User's request: <args>
//
// args 为空时省略最后一段。description 为空时省略 description 行。
std::string build_skill_invocation_hint(const SkillMetadata& meta,
                                         const std::string& args);

} // namespace acecode
