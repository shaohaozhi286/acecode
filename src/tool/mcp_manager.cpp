#include "mcp_manager.hpp"

// cpp-mcp's mcp_logger.h unconditionally defines LOG_DEBUG/LOG_INFO/LOG_ERROR
// macros that collide with our own acecode logger. Pull in mcp headers first,
// undef those macros, then include our logger so our definitions win inside
// this translation unit.
#include "mcp_stdio_client.h"
#include "mcp_message.h"
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERROR

#include "../utils/logger.hpp"

#include <cctype>
#include <exception>
#include <sstream>
#include <utility>

namespace acecode {

namespace {

// Build the single-string command cpp-mcp's stdio_client expects. We keep it
// simple: command + space-separated args. Callers needing whitespace-in-arg
// support should set up a wrapper script.
std::string build_command_line(const McpServerConfig& cfg) {
    std::ostringstream oss;
    oss << cfg.command;
    for (const auto& a : cfg.args) {
        oss << ' ' << a;
    }
    return oss.str();
}

// Convert an acecode env-map into mcp::json. mcp::json is nlohmann::ordered_json,
// which constructs happily from string key/value pairs.
mcp::json env_map_to_mcp_json(const std::map<std::string, std::string>& env) {
    mcp::json out = mcp::json::object();
    for (const auto& [k, v] : env) {
        out[k] = v;
    }
    return out;
}

// Cross-json-type bridge: acecode tools store parameters as nlohmann::json
// while cpp-mcp exposes nlohmann::ordered_json. Serialize-then-parse is
// cheaper than teaching the types about each other and avoids ODR headaches.
nlohmann::json mcp_to_std(const mcp::json& j) {
    return nlohmann::json::parse(j.dump());
}

mcp::json std_to_mcp(const nlohmann::json& j) {
    return mcp::json::parse(j.dump());
}

} // namespace

McpManager::McpManager() = default;

McpManager::~McpManager() {
    shutdown();
}

std::string McpManager::sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

bool McpManager::connect_all(const AppConfig& cfg) {
    if (cfg.mcp_servers.empty()) {
        LOG_INFO("[mcp] No MCP servers configured, skipping connection phase");
        return false;
    }

    bool any_connected = false;
    for (const auto& [name, srv_cfg] : cfg.mcp_servers) {
        const std::string cmd_line = build_command_line(srv_cfg);
        LOG_INFO("[mcp] Connecting to server '" + name + "': " + cmd_line);

        std::shared_ptr<mcp::stdio_client> client;
        try {
            client = std::make_shared<mcp::stdio_client>(
                cmd_line,
                env_map_to_mcp_json(srv_cfg.env));
        } catch (const std::exception& e) {
            LOG_ERROR("[mcp] Failed to construct stdio_client for '" + name + "': " + e.what());
            continue;
        }

        bool initialized = false;
        try {
            initialized = client->initialize("acecode", "0.1.1");
        } catch (const std::exception& e) {
            LOG_ERROR("[mcp] initialize() threw for '" + name + "': " + e.what());
            initialized = false;
        }

        if (!initialized) {
            LOG_WARN("[mcp] Skipping server '" + name + "' (initialization failed)");
            continue;
        }

        // Discover tools; failures here are non-fatal — we still keep the
        // client alive in case other code wants to use it later, but we
        // obviously can't register anything.
        std::vector<mcp::tool> tools;
        try {
            tools = client->get_tools();
        } catch (const std::exception& e) {
            LOG_WARN("[mcp] get_tools() threw for '" + name + "': " + e.what());
        }

        if (tools.empty()) {
            LOG_INFO("[mcp] Server '" + name + "' connected but exposed no tools");
        }

        for (const auto& t : tools) {
            DiscoveredTool dt;
            dt.server_name = name;
            dt.original_tool_name = t.name;
            dt.qualified_name = "mcp_" + sanitize(name) + "_" + t.name;
            dt.definition.name = dt.qualified_name;
            dt.definition.description = t.description;
            // cpp-mcp tool.parameters_schema is an ordered_json; translate.
            dt.definition.parameters = mcp_to_std(t.parameters_schema);
            discovered_tools_.push_back(std::move(dt));
        }

        ServerEntry entry;
        entry.name = name;
        entry.client = std::move(client);
        // pid is not directly observable through the public client API;
        // we rely on the client's destructor to force-kill on shutdown.
        servers_.push_back(std::move(entry));
        any_connected = true;
        LOG_INFO("[mcp] Server '" + name + "' connected with " +
                 std::to_string(tools.size()) + " tool(s)");
    }

    return any_connected;
}

void McpManager::register_tools(ToolExecutor& executor) {
    for (const auto& dt : discovered_tools_) {
        ToolImpl impl;
        impl.definition = dt.definition;
        impl.is_read_only = false; // MCP tools always require user confirmation
        impl.source = ToolSource::Mcp;

        // Capture by value so the lambda stays valid even if we ever reorder
        // discovered_tools_. `this` is captured because invocation routes
        // through the manager — the manager must outlive the executor.
        const std::string server_name = dt.server_name;
        const std::string tool_name = dt.original_tool_name;
        impl.execute = [this, server_name, tool_name](const std::string& args_json) {
            return invoke(server_name, tool_name, args_json);
        };
        executor.register_tool(impl);
    }
}

ToolResult McpManager::invoke(const std::string& server_name,
                              const std::string& tool_name,
                              const std::string& arguments_json) {
    std::shared_ptr<mcp::stdio_client> client;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& s : servers_) {
            if (s.name == server_name) {
                client = s.client;
                break;
            }
        }
    }
    if (!client) {
        return ToolResult{"[Error] MCP server '" + server_name + "' is not connected", false};
    }

    mcp::json args_mcp;
    try {
        if (arguments_json.empty()) {
            args_mcp = mcp::json::object();
        } else {
            args_mcp = mcp::json::parse(arguments_json);
        }
    } catch (const std::exception& e) {
        return ToolResult{std::string("[Error] Invalid arguments JSON: ") + e.what(), false};
    }

    try {
        mcp::json result = client->call_tool(tool_name, args_mcp);
        // The MCP tools/call result has a {"content": [...], "isError": bool}
        // shape; surface the raw JSON so the LLM can inspect structured data,
        // but if content is present as plain text, extract it for readability.
        bool is_error = false;
        if (result.contains("isError") && result["isError"].is_boolean()) {
            is_error = result["isError"].get<bool>();
        }
        std::string out;
        if (result.contains("content") && result["content"].is_array()) {
            std::ostringstream oss;
            for (const auto& item : result["content"]) {
                if (item.is_object() && item.contains("type") && item["type"] == "text" &&
                    item.contains("text") && item["text"].is_string()) {
                    if (!oss.str().empty()) oss << '\n';
                    oss << item["text"].get<std::string>();
                }
            }
            out = oss.str();
            if (out.empty()) {
                out = result.dump();
            }
        } else {
            out = result.dump();
        }
        return ToolResult{out, !is_error};
    } catch (const mcp::mcp_exception& e) {
        LOG_ERROR("[mcp] call_tool('" + server_name + "', '" + tool_name + "') mcp_exception: " + e.what());
        return ToolResult{std::string("[Error] MCP call failed: ") + e.what(), false};
    } catch (const std::exception& e) {
        LOG_ERROR("[mcp] call_tool('" + server_name + "', '" + tool_name + "') exception: " + e.what());
        return ToolResult{std::string("[Error] MCP call failed: ") + e.what(), false};
    }
}

void McpManager::kill_server(ServerEntry& entry) {
    // cpp-mcp's stdio_client destructor already force-terminates the child
    // process (Windows: TerminateProcess, POSIX: SIGTERM -> SIGKILL after a
    // short wait). Resetting the shared_ptr here drives that destructor.
    if (entry.client) {
        LOG_INFO("[mcp] Shutting down server '" + entry.name + "'");
        entry.client.reset();
    }
}

void McpManager::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    if (shutdown_done_) return;
    shutdown_done_ = true;
    for (auto& s : servers_) {
        try {
            kill_server(s);
        } catch (const std::exception& e) {
            LOG_WARN("[mcp] Exception while killing server '" + s.name + "': " + e.what());
        } catch (...) {
            LOG_WARN("[mcp] Unknown exception while killing server '" + s.name + "'");
        }
    }
    servers_.clear();
    discovered_tools_.clear();
}

std::vector<ToolDef> McpManager::get_tool_definitions() const {
    std::vector<ToolDef> defs;
    defs.reserve(discovered_tools_.size());
    for (const auto& dt : discovered_tools_) {
        defs.push_back(dt.definition);
    }
    return defs;
}

} // namespace acecode
