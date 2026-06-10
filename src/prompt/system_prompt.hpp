#pragma once

#include "../tool/tool_executor.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;
struct ProjectInstructionsConfig;
struct MemoryConfig;
struct SkillMetadata;

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

// Skill index injection (openspec/changes/inject-skill-index-into-context):
// push a compact name+description index into the per-request session context
// so the model can pattern-match user requests against installed skills.
// Without this the model has zero visibility into the skill set and never
// calls skills_list proactively.

// Character budget for the rendered index: 1% of the context window at an
// estimated 4 chars/token. Unknown window (<=0) falls back to 8000 chars.
std::size_t skills_index_char_budget(int context_window_tokens);

// Render the index grouped by category. Each entry is
// "- <name>: <description> — <when_to_use>" capped at 250 chars (UTF-8 safe).
// Over budget → all entries degrade to names-only; still over → the tail is
// cut and a "(+N more skills — call skills_list to see all)" marker appended.
std::string format_skills_index_within_budget(
    const std::vector<SkillMetadata>& skills,
    std::size_t char_budget);

// Wrap the rendered index in a titled block with a content-hash cache key.
// Null registry or empty skill list yields an empty block (not sent).
PromptContextBlock build_skills_index_context_prompt(
    const SkillRegistry* skills,
    int context_window_tokens);

PromptContextBlock build_session_context_prompt(
    const std::string& cwd,
    const MemoryRegistry* memory,
    const MemoryConfig* memory_cfg,
    const ProjectInstructionsConfig* project_instructions_cfg,
    const SkillRegistry* skills = nullptr,
    int context_window_tokens = 0);

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
