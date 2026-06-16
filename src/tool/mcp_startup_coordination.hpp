#pragma once

#include "mcp_manager.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

namespace acecode {

struct McpFirstTurnCoordinationResult {
    bool already_done = false;
    bool waited = false;
    bool settled = true;
    bool should_warn = false;
};

std::string mcp_background_start_message(std::size_t configured_count);
std::optional<std::string> mcp_status_message(const McpServerInfo& info);
std::string mcp_first_turn_still_starting_warning();

McpFirstTurnCoordinationResult coordinate_mcp_before_first_turn(
    McpManager& manager,
    std::atomic<bool>& wait_done,
    std::chrono::milliseconds wait_budget);

} // namespace acecode
