#pragma once

#include "tool_executor.hpp"

namespace acecode {

class SkillRegistry;

// `skills_list` — tier-1 progressive disclosure. Returns minimal metadata only
// (name, description, category). Read-only.
ToolImpl create_skills_list_tool(SkillRegistry& registry);

} // namespace acecode
