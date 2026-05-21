#pragma once

#include "client.hpp"
#include "tool/tool_executor.hpp"

#include <string>
#include <vector>

namespace acecode::ace_browser_bridge {

std::vector<std::string> ace_browser_core_tool_names();
std::vector<std::string> ace_browser_full_tool_names();
std::vector<std::string> ace_browser_group_names();

void register_ace_browser_bridge_tools(ToolExecutor& tools,
                                       const AceBrowserBridgeConfig& config,
                                       CliRunner runner = CliRunner{});
std::size_t unregister_ace_browser_bridge_tools(ToolExecutor& tools);

} // namespace acecode::ace_browser_bridge
