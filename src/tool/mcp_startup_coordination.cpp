#include "mcp_startup_coordination.hpp"

namespace acecode {

std::string mcp_background_start_message(std::size_t configured_count) {
    return "[MCP] Starting " + std::to_string(configured_count) +
           " server(s) in the background. External tools will appear as each server is ready.";
}

std::optional<std::string> mcp_status_message(const McpServerInfo& info) {
    switch (info.state) {
        case McpServerState::Connected:
            return "[MCP] Server '" + info.name + "' connected, registered " +
                   std::to_string(info.tool_count) + " tool(s).";
        case McpServerState::Failed:
        case McpServerState::TimedOut: {
            std::string message = "[MCP] Server '" + info.name + "' failed";
            if (!info.error.empty()) {
                message += ": " + info.error;
            }
            return message;
        }
        case McpServerState::Cancelled:
            return "[MCP] Server '" + info.name + "' startup was cancelled.";
        case McpServerState::Starting:
        case McpServerState::Disabled:
            return std::nullopt;
    }
    return std::nullopt;
}

std::string mcp_first_turn_still_starting_warning() {
    return "[MCP] Some servers are still starting; continuing with the tools that are ready.";
}

McpFirstTurnCoordinationResult coordinate_mcp_before_first_turn(
    McpManager& manager,
    std::atomic<bool>& wait_done,
    std::chrono::milliseconds wait_budget) {
    McpFirstTurnCoordinationResult result;
    if (wait_done.exchange(true)) {
        result.already_done = true;
        return result;
    }
    if (manager.configured_server_count() == 0 || !manager.has_starting_servers()) {
        return result;
    }

    result.waited = true;
    result.settled = manager.wait_for_startup_settled(wait_budget);
    result.should_warn = !result.settled && manager.has_starting_servers();
    return result;
}

} // namespace acecode
