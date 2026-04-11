#pragma once

#include "../tool/tool_executor.hpp"
#include <string>

namespace acecode {

// Build the full system prompt with identity, environment info, tool descriptions,
// and behavior rules. Regenerated each turn to ensure freshness.
std::string build_system_prompt(const ToolExecutor& tools, const std::string& cwd);

} // namespace acecode
