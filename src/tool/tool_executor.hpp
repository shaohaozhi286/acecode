#pragma once

#include "../provider/llm_provider.hpp"

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>

namespace acecode {

// Result of a tool execution
struct ToolResult {
    std::string output;
    bool success = true;
};

// Runtime context passed into a tool invocation. Optional: if left
// default-constructed, tools behave as if no streaming/abort is available.
// Populated by AgentLoop before each tool call so the tool can push
// interim output to the TUI and react to Esc-driven aborts.
struct ToolContext {
    // Called zero or more times with non-empty cleaned chunks (ANSI stripped,
    // UTF-8 boundary safe, carriage-return overwrites resolved). Only bash_tool
    // uses this currently — other tools return their output atomically.
    std::function<void(const std::string& chunk)> stream;
    // Non-owning pointer to AgentLoop::abort_requested_. Tools with long polling
    // loops must check this every iteration and terminate their subprocess /
    // work when it becomes true.
    const std::atomic<bool>* abort_flag = nullptr;
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

private:
    std::map<std::string, ToolImpl> tools_;
};

} // namespace acecode
