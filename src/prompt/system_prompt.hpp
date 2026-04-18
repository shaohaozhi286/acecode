#pragma once

#include "../tool/tool_executor.hpp"
#include <string>

namespace acecode {

class SkillRegistry;

// Build the full system prompt with identity, environment info, tool descriptions,
// and behavior rules. Regenerated each turn to ensure freshness.
// When `skills` is non-null and at least one skill is registered, a short hint
// line is appended telling the model about the skills_list / skill_view tools.
std::string build_system_prompt(const ToolExecutor& tools, const std::string& cwd,
                                const SkillRegistry* skills = nullptr);

} // namespace acecode
