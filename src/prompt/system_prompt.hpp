#pragma once

#include "../tool/tool_executor.hpp"
#include <string>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;
struct ProjectInstructionsConfig;
struct MemoryConfig;

// Build the full system prompt with identity, environment info, tool descriptions,
// and behavior rules. Regenerated each turn to ensure freshness.
// When `skills` is non-null and at least one skill is registered, a short hint
// line is appended telling the model about the skills_list / skill_view tools.
// When `memory` is non-null and MEMORY.md is non-empty, a `# User Memory`
// section with the raw index is injected. When project_instructions_cfg is
// non-null and enabled, ACECODE.md / AGENT.md / CLAUDE.md content is injected
// as a `# Project Instructions` section.
std::string build_system_prompt(const ToolExecutor& tools, const std::string& cwd,
                                const SkillRegistry* skills = nullptr,
                                const MemoryRegistry* memory = nullptr,
                                const MemoryConfig* memory_cfg = nullptr,
                                const ProjectInstructionsConfig* project_instructions_cfg = nullptr);

} // namespace acecode
