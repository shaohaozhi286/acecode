#pragma once

#include "../provider/llm_provider.hpp"
#include "diff_utils.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <optional>
#include <utility>

namespace acecode {

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
    // 结构化 diff hunk。file_edit / file_write 在产生 unified diff 文本的同时
    // 填充这个字段;TUI 用它做彩色带行号 gutter 的渲染。
    // 运行时字段 —— 不写入 session JSONL(由 session_serializer 的 allowlist
    // 天然挡住;新加字段时如果不加进白名单就不会被序列化)。
    std::optional<std::vector<DiffHunk>> hunks;
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
};

} // namespace acecode
