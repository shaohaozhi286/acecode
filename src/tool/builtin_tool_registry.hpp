#pragma once

#include "../config/config.hpp"
#include "ace_browser_bridge/browser_tools.hpp"
#include "bash_tool.hpp"
#include "file_edit_tool.hpp"
#include "file_read_tool.hpp"
#include "file_write_tool.hpp"
#include "glob_tool.hpp"
#include "goal_tool.hpp"
#include "grep_tool.hpp"
#include "task_complete_tool.hpp"
#include "tool_executor.hpp"
#include "web_search/runtime.hpp"
#include "web_search/web_search_tool.hpp"

namespace acecode {

inline void register_session_builtin_tools(ToolExecutor& tools, const AppConfig& config) {
    tools.register_tool(create_bash_tool());
    tools.register_tool(create_file_read_tool());
    tools.register_tool(create_file_write_tool());
    tools.register_tool(create_file_edit_tool());
    tools.register_tool(create_grep_tool());
    tools.register_tool(create_glob_tool());
    tools.register_tool(create_task_complete_tool());
    tools.register_tool(create_get_goal_tool());
    tools.register_tool(create_create_goal_tool());
    tools.register_tool(create_update_goal_tool());
    if (config.web_search.enabled) {
        tools.register_tool(web_search::create_web_search_tool(
            web_search::runtime().router(), web_search::runtime().cfg()));
    }
    ace_browser_bridge::register_ace_browser_bridge_tools(
        tools, config.ace_browser_bridge);
}

} // namespace acecode
