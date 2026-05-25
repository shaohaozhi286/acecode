#pragma once

#include "../tool/tool_executor.hpp"
#include <string>
#include <vector>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;
struct ProjectInstructionsConfig;
struct MemoryConfig;

struct PromptContextBlock {
    // Empty content means the block should not be sent.
    std::string content;
    // Stable key for session-local caching and diagnostics. Empty when content
    // is empty.
    std::string cache_key;
};

struct PromptCacheDiagnostics {
    std::string static_system_prompt_hash;
    std::string mutable_context_hash;
    std::string tool_schema_hash;
};

// Build the static system prompt with identity, stable environment info, and
// behavior rules. Per-request context such as current time/CWD, mutable project
// instructions, mutable memory index content, and full tool JSON schemas belong
// outside this string so provider prompt caches can reuse the static prefix
// across turns.
std::string build_system_prompt(const ToolExecutor& tools, const std::string& cwd,
                                const SkillRegistry* skills = nullptr,
                                const MemoryRegistry* memory = nullptr,
                                const MemoryConfig* memory_cfg = nullptr,
                                const ProjectInstructionsConfig* project_instructions_cfg = nullptr);

// Build provider-visible, session-scoped context blocks. These are assembled
// for the current API request only and must not be persisted into the visible
// transcript.
PromptContextBlock build_project_instructions_context_prompt(
    const std::string& cwd,
    const ProjectInstructionsConfig* cfg);
PromptContextBlock build_user_memory_context_prompt(
    const MemoryRegistry* memory,
    const MemoryConfig* cfg);
PromptContextBlock build_session_context_prompt(
    const std::string& cwd,
    const MemoryRegistry* memory,
    const MemoryConfig* memory_cfg,
    const ProjectInstructionsConfig* project_instructions_cfg);

// Build dynamic request-local context. This is sent near the end of the
// messages array for the current provider call only; it must not be persisted
// into session history.
std::string build_request_context_prompt(const std::string& cwd);

// Deterministic helpers for prompt-cache diagnostics and tests.
std::string prompt_component_hash(const std::string& text);
std::string serialize_tool_schemas_for_prompt_cache(const std::vector<ToolDef>& tools);
PromptCacheDiagnostics build_prompt_cache_diagnostics(
    const std::string& static_system_prompt,
    const std::string& mutable_context,
    const std::vector<ToolDef>& tools);

} // namespace acecode
