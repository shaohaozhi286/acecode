#pragma once

#include "tool_executor.hpp"

namespace acecode {

ToolImpl create_get_goal_tool();
ToolImpl create_create_goal_tool();
ToolImpl create_update_goal_tool();

} // namespace acecode
