#pragma once

#include "../provider/llm_provider.hpp"

#include <string>
#include <vector>
#include <map>
#include <functional>

namespace acecode {

// Result of a tool execution
struct ToolResult {
    std::string output;
    bool success = true;
};

// Origin of a registered tool. MCP tools are grouped separately in the system
// prompt so the LLM can distinguish internal versus external capabilities.
enum class ToolSource {
    Builtin = 0,
    Mcp = 1,
};

// A registered tool implementation
struct ToolImpl {
    ToolDef definition;
    std::function<ToolResult(const std::string& arguments_json)> execute;
    bool is_read_only = false; // Read-only tools are auto-approved without user confirmation
    ToolSource source = ToolSource::Builtin;
};

class ToolExecutor {
public:
    void register_tool(const ToolImpl& tool);

    // Get all tool definitions for inclusion in API requests
    std::vector<ToolDef> get_tool_definitions() const;

    // Get tool definitions filtered by source (built-in vs MCP).
    std::vector<ToolDef> get_tool_definitions_by_source(ToolSource source) const;

    // Execute a tool call and return the result
    ToolResult execute(const std::string& tool_name, const std::string& arguments_json) const;

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
