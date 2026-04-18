#pragma once

#include "../config/config.hpp"
#include "tool_executor.hpp"
#include "../provider/llm_provider.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Forward-declare the cpp-mcp client classes so mcp headers don't leak into
// wider compilation units. The full definitions are only needed inside the .cpp.
namespace mcp {
    class client;
    class stdio_client;
    class sse_client;
}

namespace acecode {

// Runtime state of an MCP server tracked by McpManager.
enum class McpServerState {
    Connected = 0, // child process running, tools registered
    Disabled  = 1, // user-stopped via /mcp disable; tools unregistered
    Failed    = 2, // last connect/reconnect attempt failed
};

// Lightweight summary of one MCP server, returned by list_servers() for the
// /mcp command.
struct McpServerInfo {
    std::string name;
    McpServerState state;
    size_t tool_count;
    // Human-readable transport tag: "stdio", "sse" or "http".
    std::string transport;
    // For stdio: joined "command args..." as configured.
    // For sse/http: "<url><sse_endpoint>".
    std::string command_line;
};

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
    size_t connected_server_count() const;

    // Count of tools discovered across all servers.
    size_t discovered_tool_count() const { return discovered_tools_.size(); }

    // Runtime control surface for /mcp slash command. All take a ToolExecutor
    // ref because changes need to propagate to the registered tool set.
    // Each returns true on a state-changing success.

    // Stop the named server's child process and unregister its tools.
    bool disable(const std::string& name, ToolExecutor& executor);

    // Reconnect a previously-disabled or failed server (no-op if Connected).
    bool enable(const std::string& name, ToolExecutor& executor);

    // Force a teardown + reconnect, regardless of current state.
    bool reconnect(const std::string& name, ToolExecutor& executor);

    // Snapshot of all known servers in registration order.
    std::vector<McpServerInfo> list_servers() const;

    // Tools grouped by server (preserves discovery order). Servers without
    // any registered tools (Disabled / Failed / empty) appear with an empty
    // vector so callers can render a status line for them.
    std::vector<std::pair<std::string, std::vector<ToolDef>>> list_tools_by_server() const;

    // Whether the named server is recorded in the manager.
    bool has_server(const std::string& name) const;

    // Names of every recorded server (for diagnostic output).
    std::vector<std::string> server_names() const;

private:
    struct ServerEntry {
        std::string name;
        McpServerConfig cfg;                 // remembered so reconnect needs only the name
        std::string command_line;            // human-readable locator (command line or url)
        std::shared_ptr<mcp::client> client; // stdio_client or sse_client via base interface
        McpServerState state = McpServerState::Failed;
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

    // Locate an entry by name. Returns nullptr if missing. Caller must hold mu_.
    ServerEntry* find_entry_locked(const std::string& name);

    // Tear down a single server: stop child, drop tools from discovered_tools_,
    // unregister them from executor. Caller must hold mu_.
    void teardown_locked(ServerEntry& entry, ToolExecutor& executor);

    // Connect & register tools for a single entry. Sets entry.state. Returns
    // true on success. Caller must hold mu_.
    bool connect_entry_locked(ServerEntry& entry, ToolExecutor& executor);

    mutable std::mutex mu_;
    std::vector<ServerEntry> servers_;
    std::vector<DiscoveredTool> discovered_tools_;
    bool shutdown_done_ = false;
};

} // namespace acecode
