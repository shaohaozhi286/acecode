#include "tool_executor.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include <sstream>

namespace acecode {

void ToolExecutor::register_tool(const ToolImpl& tool) {
    LOG_INFO("Registering tool: " + tool.definition.name);
    tools_[tool.definition.name] = tool;
}

bool ToolExecutor::unregister_tool(const std::string& name) {
    auto it = tools_.find(name);
    if (it == tools_.end()) return false;
    LOG_INFO("Unregistering tool: " + name);
    tools_.erase(it);
    return true;
}

std::vector<ToolDef> ToolExecutor::get_tool_definitions() const {
    std::vector<ToolDef> defs;
    for (const auto& [name, impl] : tools_) {
        defs.push_back(impl.definition);
    }
    return defs;
}

std::vector<ToolDef> ToolExecutor::get_tool_definitions_by_source(ToolSource source) const {
    std::vector<ToolDef> defs;
    for (const auto& [name, impl] : tools_) {
        if (impl.source == source) defs.push_back(impl.definition);
    }
    return defs;
}

ToolResult ToolExecutor::execute(const std::string& tool_name, const std::string& arguments_json) const {
    return execute(tool_name, arguments_json, ToolContext{});
}

ToolResult ToolExecutor::execute(const std::string& tool_name, const std::string& arguments_json,
                                 const ToolContext& ctx) const {
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
        LOG_ERROR("execute: unknown tool '" + tool_name + "'");
        return ToolResult{"[Error] Unknown tool: " + tool_name, false};
    }
    LOG_DEBUG("execute: " + tool_name + " args=" + log_truncate(arguments_json, 300));
    auto result = it->second.execute(arguments_json, ctx);
    LOG_DEBUG("execute result: success=" + std::string(result.success ? "true" : "false") + " len=" + std::to_string(result.output.size()));
    return result;
}

bool ToolExecutor::has_tool(const std::string& name) const {
    return tools_.find(name) != tools_.end();
}

bool ToolExecutor::is_read_only(const std::string& name) const {
    auto it = tools_.find(name);
    return it != tools_.end() && it->second.is_read_only;
}

std::string ToolExecutor::generate_tools_prompt() const {
    std::ostringstream oss;
    for (const auto& [name, impl] : tools_) {
        oss << "## " << impl.definition.name << "\n"
            << "Description: " << impl.definition.description << "\n"
            << "Parameters:\n```json\n"
            << impl.definition.parameters.dump(2) << "\n```\n\n";
    }
    return oss.str();
}

ChatMessage ToolExecutor::format_tool_result(const std::string& tool_call_id, const ToolResult& result) {
    ChatMessage msg;
    msg.role = "tool";
    msg.content = ensure_utf8(result.output);
    msg.tool_call_id = tool_call_id;
    return msg;
}

ChatMessage ToolExecutor::format_assistant_tool_calls(const ChatResponse& response) {
    ChatMessage msg;
    msg.role = "assistant";
    msg.content = response.content;

    nlohmann::json tc_array = nlohmann::json::array();
    for (const auto& tc : response.tool_calls) {
        nlohmann::json tc_obj;
        tc_obj["id"] = tc.id;
        tc_obj["type"] = "function";
        tc_obj["function"]["name"] = tc.function_name;
        tc_obj["function"]["arguments"] = tc.function_arguments;
        tc_array.push_back(tc_obj);
    }
    msg.tool_calls = tc_array;

    return msg;
}

} // namespace acecode
