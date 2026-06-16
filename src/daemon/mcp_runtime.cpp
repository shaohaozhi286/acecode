#include "mcp_runtime.hpp"

#include "../utils/logger.hpp"

namespace acecode::daemon {

DaemonMcpRuntime::~DaemonMcpRuntime() {
    shutdown();
}

bool DaemonMcpRuntime::start(const AppConfig& cfg, ToolExecutor& tools) {
    if (!manager_.connect_all(cfg)) {
        return false;
    }

    LOG_INFO("[daemon:mcp] starting " +
             std::to_string(manager_.configured_server_count()) +
             " server(s) in the background");
    manager_.set_status_callback([](const acecode::McpServerInfo& info) {
        if (info.state == acecode::McpServerState::Connected) {
            LOG_INFO("[daemon:mcp] server '" + info.name + "' connected with " +
                     std::to_string(info.tool_count) + " tool(s)");
        } else if (info.state == acecode::McpServerState::Failed ||
                   info.state == acecode::McpServerState::TimedOut) {
            LOG_WARN("[daemon:mcp] server '" + info.name + "' failed" +
                     (info.error.empty() ? std::string{} : (": " + info.error)));
        }
    });
    manager_.start_async(tools);
    return true;
}

void DaemonMcpRuntime::shutdown() {
    manager_.shutdown();
}

} // namespace acecode::daemon
