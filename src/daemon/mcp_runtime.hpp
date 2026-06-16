#pragma once

#include "../config/config.hpp"
#include "../tool/mcp_manager.hpp"
#include "../tool/tool_executor.hpp"

namespace acecode::daemon {

class DaemonMcpRuntime {
public:
    DaemonMcpRuntime() = default;
    ~DaemonMcpRuntime();

    DaemonMcpRuntime(const DaemonMcpRuntime&) = delete;
    DaemonMcpRuntime& operator=(const DaemonMcpRuntime&) = delete;

    bool start(const AppConfig& cfg, ToolExecutor& tools);
    void shutdown();

    McpManager& manager() { return manager_; }
    const McpManager& manager() const { return manager_; }

private:
    McpManager manager_;
};

} // namespace acecode::daemon
