#pragma once

#include "tool_executor.hpp"

#include <memory>

namespace acecode {

class SessionControlService;

struct SessionControlToolDeps {
    std::shared_ptr<SessionControlService> service;
};

ToolImpl create_session_query_tool(
    std::shared_ptr<SessionControlToolDeps> deps);
ToolImpl create_session_control_tool(
    std::shared_ptr<SessionControlToolDeps> deps);

} // namespace acecode
