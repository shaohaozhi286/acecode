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

// A registered tool implementation
struct ToolImpl {
    ToolDef definition;
    std::function<ToolResult(const std::string& arguments_json)> execute;
};

class ToolExecutor {
public:
    void register_tool(const ToolImpl& tool);

    // Get all tool definitions for inclusion in API requests
    std::vector<ToolDef> get_tool_definitions() const;

    // Execute a tool call and return the result
    ToolResult execute(const std::string& tool_name, const std::string& arguments_json) const;

    // Check if a tool is registered
    bool has_tool(const std::string& name) const;

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
