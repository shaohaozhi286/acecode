#pragma once

#include "../provider/llm_provider.hpp"
#include "diff_utils.hpp"
#include "question_policy.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <optional>
#include <utility>
#include <mutex>

namespace acecode {

class SessionManager;
class ToolExecutor;

// Structured summary used by the TUI to render a single-line tool-result row
// (icon + verb + object + dot-separated metrics). Unset on tools that have not
// opted in; the TUI then falls back to the legacy 10-line fold path.
struct ToolSummary {
    std::string verb;     // "Ran" / "Read" / "Wrote" / "Created" / "Edited" ...
    std::string object;   // file path or command preview
    std::vector<std::pair<std::string, std::string>> metrics; // ordered
    std::string icon;     // short glyph (may be Unicode or ASCII fallback)
};

// Result of a tool execution
struct ToolResult {
    std::string output;
    bool success = true;
    std::optional<ToolSummary> summary; // populated by tools that opt in
    // Optional UI/persistence metadata. This is never part of provider-visible
    // text; AgentLoop stores it on the ChatMessage and web lifecycle payloads.
    nlohmann::json metadata = nlohmann::json::object();
    // Optional user-role prompt to append after this tool result. This is used
    // for progressive capability disclosure that should affect only the active
    // conversation after a tool is explicitly opened, rather than the global
    // cacheable system prompt.
    std::optional<std::string> post_user_prompt;
    std::string post_user_prompt_display_text;
    // 结构化 diff hunk。file_edit / file_write 在产生 unified diff 文本的同时
    // 填充这个字段;TUI 用它做彩色带行号 gutter 的渲染。
    // 运行时字段 —— 不写入 session JSONL(由 session_serializer 的 allowlist
    // 天然挡住;新加字段时如果不加进白名单就不会被序列化)。
    std::optional<std::vector<DiffHunk>> hunks;
    // Structured output attachments produced by a tool. Items are either stored
    // AttachmentRecord JSON objects or pre-materialization descriptors such as
    // {name,mime_type,data_url} / {name,mime_type,path}. AgentLoop materializes
    // descriptors into session attachments before events and JSONL persistence.
    nlohmann::json attachments = nlohmann::json::array();
    std::vector<std::string> attachment_warnings;

    bool has_attachments() const {
        return attachments.is_array() && !attachments.empty();
    }
};

// Runtime context passed into a tool invocation. Optional: if left
// default-constructed, tools behave as if no streaming/abort is available.
// Populated by AgentLoop before each tool call so the tool can push
// interim output to the TUI and react to Esc-driven aborts.
struct ToolContext {
    // Session workspace cwd. Tools that support a default working directory
    // should prefer this over the daemon process cwd when their own arguments
    // omit a cwd/path.
    std::string cwd;

    // Called zero or more times with non-empty cleaned chunks (ANSI stripped,
    // UTF-8 boundary safe, carriage-return overwrites resolved). Only bash_tool
    // uses this currently — other tools return their output atomically.
    std::function<void(const std::string& chunk)> stream;
    // Non-owning pointer to AgentLoop::abort_requested_. Tools with long polling
    // loops must check this every iteration and terminate their subprocess /
    // work when it becomes true.
    const std::atomic<bool>* abort_flag = nullptr;
    // Optional file-checkpoint hook used by write tools. Tools call this after
    // validation succeeds and immediately before mutating a file so /rewind can
    // restore the pre-write state.
    std::function<void(const std::string& path)> track_file_write_before;

    // Per-session scratch directory for temporary helper files. AgentLoop
    // injects `.acecode/tmp/session-<id>` under the workspace when a session id
    // is available. Shell tools expose this as ACECODE_TMPDIR.
    std::string scratch_dir;

    // Optional async channel for AskUserQuestion. Daemon path injects an impl
    // backed by AskUserQuestionPrompter; TUI path keeps it null and registers
    // the TUI-flavored AskUserQuestion factory which talks to TuiState directly.
    //
    // Wire format (nlohmann::json) — kept loose so this header doesn't pull
    // in session/ headers:
    //   in  questions_payload: array of {id, text, options:[{label, value}], multiSelect}
    //   out: { cancelled: bool,
    //          answers: [ { question_id, selected: [str], custom_text: str } ] }
    // Empty function = AskUserQuestion tool returns the rejected ToolResult.
    std::function<nlohmann::json(const nlohmann::json& questions_payload)> ask_user_questions;

    // Per-session state injected by AgentLoop. Goal tools use this instead of
    // binding to one SessionManager at process-wide tool registration time.
    SessionManager* session_manager = nullptr;

    // AgentLoop sets this so bash can hand the full output to the
    // tool-result budget layer. Standalone tool callers keep the legacy
    // 100KB inline cap unless they explicitly opt in.
    bool preserve_full_output = false;

    std::function<void()> account_goal_usage;
    std::function<void(const nlohmann::json& goal_payload)> emit_goal_updated;
    std::function<void(const std::string& session_id)> emit_goal_cleared;
    std::function<void(const nlohmann::json& todo_payload)> emit_todo_updated;

    // Goal 无人值守模式探针(AgentLoop 注入)。true = 当前会话(或父会话)
    // 有 Active goal 且非 Plan mode,所有需要用户确认的交互必须自动进行:
    // AskUserQuestion 不弹 UI,直接返回「自行决策并继续」的自动应答。
    // 空函数 = 正常交互模式。
    std::function<bool()> goal_unattended_active;

    // AskUserQuestion 应答策略探针(AgentLoop 注入,模式同
    // goal_unattended_active)。返回 resolve_question_policy 的解析结果,
    // 每次调用实时反映权限模式(/yolo 会话中切换后下一次提问即生效)。
    // 空函数 = Ask(独立调用 ToolExecutor 的旧行为)。优先级低于
    // goal_unattended_active。见 src/tool/question_policy.hpp。
    std::function<ResolvedQuestionPolicy()> question_policy;

    // Plan-mode tools use these callbacks to mutate the active AgentLoop's
    // permission state. They are callbacks rather than direct PermissionManager
    // references so the tool layer stays independent of the TUI/daemon runtime.
    std::function<std::string()> current_permission_mode;
    std::function<std::string()> enter_plan_mode;
    std::function<std::string()> exit_plan_mode;

    // Runtime access to the active executor. Tools that intentionally change
    // the available tool set can use this to register additional tools for the
    // next model request.
    ToolExecutor* tool_executor = nullptr;
};

// Origin of a registered tool. MCP tools are grouped separately in the system
// prompt so the LLM can distinguish internal versus external capabilities.
enum class ToolSource {
    Builtin = 0,
    Mcp = 1,
};

// A registered tool implementation. The execute function takes a ToolContext —
// tools that don't need streaming simply ignore it.
struct ToolImpl {
    ToolDef definition;
    std::function<ToolResult(const std::string& arguments_json, const ToolContext& ctx)> execute;
    bool is_read_only = false; // Read-only tools are auto-approved without user confirmation
    ToolSource source = ToolSource::Builtin;
};

class ToolExecutor {
public:
    void register_tool(const ToolImpl& tool);

    // Remove a tool by name. Returns true if the tool existed and was removed.
    bool unregister_tool(const std::string& name);

    // Get all tool definitions for inclusion in API requests
    std::vector<ToolDef> get_tool_definitions() const;

    // Get tool definitions filtered by source (built-in vs MCP).
    std::vector<ToolDef> get_tool_definitions_by_source(ToolSource source) const;

    // Execute a tool call and return the result. Legacy overload — no streaming,
    // no abort flag. Delegates to the ctx overload with a default context.
    ToolResult execute(const std::string& tool_name, const std::string& arguments_json) const;

    // Execute with a ToolContext. Tools that support streaming will call
    // ctx.stream() as chunks arrive; tools that support cancellation will
    // poll ctx.abort_flag.
    ToolResult execute(const std::string& tool_name, const std::string& arguments_json,
                       const ToolContext& ctx) const;

    // Check if a tool is registered
    bool has_tool(const std::string& name) const;

    // Check if a tool is read-only (auto-approved)
    bool is_read_only(const std::string& name) const;

    // Generate a formatted description of all registered tools for system prompt
    std::string generate_tools_prompt() const;

    // Format a tool result into a ChatMessage suitable for the messages array
    static ChatMessage format_tool_result(const std::string& tool_call_id, const ToolResult& result);

    // Format an assistant message that includes tool calls (from the API response)
    static ChatMessage format_assistant_tool_calls(const ChatResponse& response);

    // Build a compact one-line preview for a tool_call row. For bash takes the
    // command's first 60 chars; for file_read/file_write/file_edit takes the
    // file_path (tail-truncated to 40 chars); other tools return an empty
    // string so the TUI falls back to the legacy `[Tool: X] {JSON}` format.
    static std::string build_tool_call_preview(const std::string& tool_name,
                                               const std::string& arguments_json);

private:
    std::map<std::string, ToolImpl> tools_;
    mutable std::mutex tools_mu_;
};

} // namespace acecode
