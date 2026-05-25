#pragma once

#include "../tool/tool_executor.hpp"
#include <string>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;
struct ProjectInstructionsConfig;
struct MemoryConfig;

// Build the static system prompt with identity, stable environment info, tool
// descriptions, and behavior rules. Per-request context such as current time
// and CWD belongs in build_request_context_prompt() so provider prompt caches
// can reuse this prefix across turns.
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

// Build dynamic request-local context. This is sent near the end of the
// messages array for the current provider call only; it must not be persisted
// into session history.
std::string build_request_context_prompt(const std::string& cwd);

} // namespace acecode
