#pragma once

#include "../tool/tool_executor.hpp"
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace acecode {

class SkillRegistry;
struct ExpertDefinition;
class MemoryRegistry;
struct ProjectInstructionsConfig;
struct CustomInstructionsConfig;
struct MemoryConfig;
struct SkillMetadata;

struct PromptContextBlock {
    // Empty content means the block should not be sent.
    std::string content;
    // Stable key for session-local caching and diagnostics. Empty when content
    // is empty.
    std::string cache_key;
    // Optional diagnostic emitted when a new cached revision is installed.
    std::string warning;
};

enum class SkillMetadataBudgetUnit {
    Tokens,
    Characters,
};

struct SkillMetadataBudget {
    SkillMetadataBudgetUnit unit = SkillMetadataBudgetUnit::Characters;
    std::size_t limit = 0;
};

struct SkillIndexRenderReport {
    std::size_t total_count = 0;
    std::size_t included_count = 0;
    std::size_t omitted_count = 0;
    std::size_t truncated_description_chars = 0;
    std::size_t truncated_description_count = 0;

    std::string warning_message() const;
};

struct SkillIndexRenderResult {
    std::string content;
    // Optional Codex-style root alias table rendered before Available skills.
    std::vector<std::string> skill_root_lines;
    SkillIndexRenderReport report;
};

struct PromptCacheDiagnostics {
    std::string static_system_prompt_hash;
    std::string mutable_context_hash;
    std::string tool_schema_hash;
};

struct PromptContextCategoryBytes {
    std::size_t project_rules = 0;
    std::size_t skills = 0;
    std::size_t dynamic_context = 0;
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
                                const ProjectInstructionsConfig* project_instructions_cfg = nullptr,
                                const ToolCapabilityPolicy* effective_tool_policy = nullptr);

// Build provider-visible, session-scoped context blocks. These are assembled
// for the current API request only and must not be persisted into the visible
// transcript.
PromptContextBlock build_project_instructions_context_prompt(
    const std::string& cwd,
    const ProjectInstructionsConfig* cfg);
PromptContextBlock build_user_memory_context_prompt(
    const MemoryRegistry* memory,
    const MemoryConfig* cfg);
PromptContextBlock build_custom_instructions_context_prompt(
    const CustomInstructionsConfig* cfg);

// Skill index injection (openspec/changes/adopt-codex-skill-catalog):
// push a compact name+description+source-locator index into a dedicated,
// request-local high-priority context message so the model can pattern-match
// user requests against installed skills.
// Without this the model has zero visibility into the skill set and never
// calls skills_list proactively.

// Codex-compatible metadata budget: 2% of a known context window in tokens.
// Unknown windows (<=0) fall back to 8000 Unicode characters.
SkillMetadataBudget skills_index_budget(int context_window_tokens);

// Render Codex-compatible flat entries containing name, description, and a
// `file:` locator. Descriptions are capped at 1024 Unicode code points. If the
// full index does not fit, description space is allocated round-robin across
// every skill before any skill is omitted; a root-alias form is selected only
// when it improves the bounded result.
SkillIndexRenderResult format_skills_index_within_budget(
    const std::vector<SkillMetadata>& skills,
    SkillMetadataBudget budget,
    bool skills_list_available = true);

// Wrap the rendered index in a titled block with a content-hash cache key.
// Null registry or empty skill list yields an empty block (not sent).
// `dormant_names` (optional): skill names to hide from the rendered index
// (dormant skills stay available via explicit mention but are not listed).
PromptContextBlock build_skills_index_context_prompt(
    const SkillRegistry* skills,
    int context_window_tokens,
    bool skill_view_available = true,
    bool skills_list_available = true,
    const std::set<std::string>* dormant_names = nullptr);

// gitStatus 快照块(openspec add-git-context):把 collector 采集的快照文本
// 包成带缓存 key 的块。空文本(非仓库/采集失败/disabled)→ 空块不发送。
// 快照按会话缓存由调用方(AgentLoop)负责,这里只做包装。
PromptContextBlock build_git_status_context_prompt(
    const std::string& snapshot_text);

PromptContextBlock build_expert_context_prompt(
    const ExpertDefinition* expert,
    const std::string& member_id = std::string(),
    bool spawn_subagent_available = true);

PromptContextBlock build_session_context_prompt(
    const std::string& cwd,
    const MemoryRegistry* memory,
    const MemoryConfig* memory_cfg,
    const ProjectInstructionsConfig* project_instructions_cfg,
    const SkillRegistry* skills = nullptr,
    int context_window_tokens = 0,
    const CustomInstructionsConfig* custom_instructions_cfg = nullptr,
    const std::string& git_status_snapshot = std::string(),
    const ExpertDefinition* expert = nullptr,
    const std::string& expert_member_id = std::string(),
    PromptContextCategoryBytes* category_bytes = nullptr,
    bool skill_view_available = true,
    bool skills_list_available = true,
    bool spawn_subagent_available = true,
    bool include_skill_index = false);

// Build one-turn proactive delegation guidance. Empty output means swarm mode
// is disabled or the effective tool policy cannot expose spawn_subagent.
std::string build_swarm_mode_context_prompt(
    bool enabled,
    bool spawn_subagent_available);

// Deterministic helpers for prompt-cache diagnostics and tests.
std::string prompt_component_hash(const std::string& text);
std::string serialize_tool_schemas_for_prompt_cache(const std::vector<ToolDef>& tools);
PromptCacheDiagnostics build_prompt_cache_diagnostics(
    const std::string& static_system_prompt,
    const std::string& mutable_context,
    const std::vector<ToolDef>& tools);

} // namespace acecode
