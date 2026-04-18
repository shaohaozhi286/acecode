#pragma once

#include "tool_executor.hpp"

namespace acecode {

class SkillRegistry;

// `skill_view` — tier-2/3 progressive disclosure. Loads the full SKILL.md body
// for a given skill, or (when file_path is provided) a supporting file inside
// the skill directory. Read-only.
ToolImpl create_skill_view_tool(SkillRegistry& registry);

} // namespace acecode
