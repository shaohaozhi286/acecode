#pragma once

#include "../config/config.hpp"
#include "tool_executor.hpp"
#include "../provider/llm_provider.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Forward-declare cpp-mcp's stdio_client so mcp headers don't leak into
// wider compilation units. The full definition is only needed inside the .cpp.
namespace mcp { class stdio_client; }

namespace acecode {

// McpManager owns the lifetime of every configured MCP stdio server, discovers
// the tools they expose, and bridges tool invocations back to the acecode
// ToolExecutor.
//
// The manager takes no ownership of the ToolExecutor; callers are expected to
// keep the executor alive for at least as long as this manager.
class McpManager {
public:
    McpManager();
    ~McpManager();

    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;

    // Connect to every server in cfg.mcp_servers. Failures for individual
    // servers are logged and skipped. Safe to call when cfg.mcp_servers is
    // empty — no work is done. Returns true if at least one server connected.
    bool connect_all(const AppConfig& cfg);

    // Discover tools from each connected server and register them into
    // executor with the `mcp_{server}_{tool}` naming convention. All MCP
    // tools are registered as non-read-only (permission required).
    void register_tools(ToolExecutor& executor);

    // Terminate all child processes. Safe to call multiple times and from
    // shutdown paths; absorbs errors from already-dead processes.
    void shutdown();

    // Introspection: definitions of every tool discovered, in registration order.
    // Useful for diagnostics / tests; the authoritative copy lives in ToolExecutor.
    std::vector<ToolDef> get_tool_definitions() const;

    // Count of successfully connected servers.
    size_t connected_server_count() const { return servers_.size(); }

    // Count of tools discovered across all servers.
    size_t discovered_tool_count() const { return discovered_tools_.size(); }

private:
    struct ServerEntry {
        std::string name;
        std::shared_ptr<mcp::stdio_client> client;
        int pid = -1;           // POSIX: actual pid; Windows: same field holds pid (handle tracked separately)
#ifdef _WIN32
        void* process_handle = nullptr; // Windows HANDLE; kept opaque to avoid leaking windows.h
#endif
    };

    struct DiscoveredTool {
        std::string qualified_name;        // e.g. "mcp_github_search_issues"
        std::string server_name;           // original server key
        std::string original_tool_name;    // original tool name as returned by MCP
        ToolDef definition;                // parameters schema + description
    };

    // Build the qualified tool name. Sanitizes non-alphanumeric characters in
    // the server name to underscore so function-calling naming rules are met.
    static std::string sanitize(const std::string& s);

    // Invoke a tool via the owning server. Thread-safe with respect to other
    // invocations of the same server (cpp-mcp handles the pipe locking).
    ToolResult invoke(const std::string& server_name,
                      const std::string& tool_name,
                      const std::string& arguments_json);

    // Force-kill a single server's subprocess. No-op if the process is gone.
    static void kill_server(ServerEntry& entry);

    mutable std::mutex mu_;
    std::vector<ServerEntry> servers_;
    std::vector<DiscoveredTool> discovered_tools_;
    bool shutdown_done_ = false;
};

} // namespace acecode
