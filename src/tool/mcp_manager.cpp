#include "mcp_manager.hpp"

// cpp-mcp's mcp_logger.h unconditionally defines LOG_DEBUG/LOG_INFO/LOG_ERROR
// macros that collide with our own acecode logger. Pull in mcp headers first,
// undef those macros, then include our logger so our definitions win inside
// this translation unit.
//
// Include mcp_sse_client.h before mcp_stdio_client.h: the former transitively
// pulls in httplib.h which needs winsock2.h on Windows, and winsock2.h must
// precede windows.h (included by mcp_stdio_client.h) to avoid redefinitions
// from the legacy winsock.h that <windows.h> drags in.
// mcp_streamable_http_client.h uses PIMPL and does not drag in httplib, so
// its position in this ordering is not load-bearing.
#include "mcp_sse_client.h"
#include "mcp_streamable_http_client.h"
#include "mcp_stdio_client.h"
#include "mcp_message.h"
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERROR

#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>
#include <utility>

namespace acecode {

namespace {

// Short tag used in McpServerInfo and log prefixes.
const char* transport_tag(McpTransport t) {
    switch (t) {
        case McpTransport::Stdio: return "stdio";
        case McpTransport::Sse:   return "sse";
        case McpTransport::Http:  return "http";
    }
    return "stdio";
}

// Build the single-string command cpp-mcp's stdio_client expects. We keep it
// simple: command + space-separated args. Callers needing whitespace-in-arg
// support should set up a wrapper script.
std::string build_stdio_command_line(const McpServerConfig& cfg) {
    std::ostringstream oss;
    oss << cfg.command;
    for (const auto& a : cfg.args) {
        oss << ' ' << a;
    }
    return oss.str();
}

// Human-readable locator for sse/http entries.
std::string build_http_locator(const McpServerConfig& cfg) {
    std::string out = cfg.url;
    std::string ep = cfg.sse_endpoint;
    if (cfg.transport == McpTransport::Http && (ep.empty() || ep == "/sse")) {
        // "sse_endpoint" default is "/sse"; for Streamable HTTP show /mcp instead
        // so the displayed locator matches what the client will actually POST to.
        ep = "/mcp";
    }
    if (!ep.empty()) {
        if (out.empty() || out.back() != '/' || ep.front() != '/') {
            out += ep;
        } else {
            out += ep.substr(1);
        }
    }
    return out;
}

std::string build_locator(const McpServerConfig& cfg) {
    return cfg.transport == McpTransport::Stdio
        ? build_stdio_command_line(cfg)
        : build_http_locator(cfg);
}

// Turn `headers` map into a "k1,k2,k3" listing for safe logging (no values).
std::string header_keys_summary(const std::map<std::string, std::string>& headers) {
    if (headers.empty()) return "";
    std::string out;
    bool first = true;
    for (const auto& [k, _] : headers) {
        if (!first) out += ",";
        out += k;
        first = false;
    }
    return out;
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

McpManager::ServerEntry* McpManager::find_entry_locked(const std::string& name) {
    for (auto& e : servers_) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

bool McpManager::connect_entry_locked(ServerEntry& entry, ToolExecutor& executor) {
    const std::string tag = std::string("[mcp:") + transport_tag(entry.cfg.transport) + "] ";
    LOG_INFO(tag + "Connecting to server '" + entry.name + "': " + entry.command_line);

    std::shared_ptr<mcp::client> client;
    try {
        if (entry.cfg.transport == McpTransport::Stdio) {
            client = std::make_shared<mcp::stdio_client>(
                entry.command_line,
                env_map_to_mcp_json(entry.cfg.env));
        } else if (entry.cfg.transport == McpTransport::Sse) {
            auto sse = std::make_shared<mcp::sse_client>(
                entry.cfg.url,
                entry.cfg.sse_endpoint,
                entry.cfg.validate_certificates,
                entry.cfg.ca_cert_path);
            for (const auto& [k, v] : entry.cfg.headers) {
                sse->set_header(k, v);
            }
            if (!entry.cfg.auth_token.empty()) {
                sse->set_auth_token(entry.cfg.auth_token);
            }
            if (entry.cfg.timeout_seconds > 0) {
                sse->set_timeout(entry.cfg.timeout_seconds);
            }
            if (!entry.cfg.validate_certificates) {
                LOG_WARN(tag + "WARNING: certificate validation disabled for '" + entry.name + "'");
            }
            const std::string auth_state = entry.cfg.auth_token.empty() ? "none" : "present";
            std::string hdr_keys = header_keys_summary(entry.cfg.headers);
            LOG_INFO(tag + "sse config for '" + entry.name +
                     "' auth=" + auth_state +
                     (hdr_keys.empty() ? "" : (" header_keys=" + hdr_keys)));
            client = std::move(sse);
        } else {
            // Streamable HTTP (2025-03-26): single POST endpoint, typically /mcp.
            // Reuse sse_endpoint field as the endpoint path; default to "/mcp"
            // when the user didn't customize it.
            std::string ep = entry.cfg.sse_endpoint;
            if (ep.empty() || ep == "/sse") {
                ep = "/mcp";
            }
            auto h = std::make_shared<mcp::streamable_http_client>(
                entry.cfg.url,
                ep,
                entry.cfg.validate_certificates,
                entry.cfg.ca_cert_path);
            for (const auto& [k, v] : entry.cfg.headers) {
                h->set_header(k, v);
            }
            if (!entry.cfg.auth_token.empty()) {
                h->set_auth_token(entry.cfg.auth_token);
            }
            if (entry.cfg.timeout_seconds > 0) {
                h->set_timeout(entry.cfg.timeout_seconds);
            }
            if (!entry.cfg.validate_certificates) {
                LOG_WARN(tag + "WARNING: certificate validation disabled for '" + entry.name + "'");
            }
            const std::string auth_state = entry.cfg.auth_token.empty() ? "none" : "present";
            std::string hdr_keys = header_keys_summary(entry.cfg.headers);
            LOG_INFO(tag + "http (streamable) config for '" + entry.name +
                     "' endpoint=" + ep +
                     " auth=" + auth_state +
                     (hdr_keys.empty() ? "" : (" header_keys=" + hdr_keys)));
            client = std::move(h);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(tag + "Failed to construct client for '" + entry.name + "': " + e.what());
        entry.state = McpServerState::Failed;
        return false;
    }

    bool initialized = false;
    try {
        initialized = client->initialize("acecode", "0.1.1");
    } catch (const std::exception& e) {
        LOG_ERROR(tag + "initialize() threw for '" + entry.name + "': " + e.what());
        initialized = false;
    }

    if (!initialized) {
        LOG_WARN(tag + "Skipping server '" + entry.name + "' (initialization failed)");
        entry.state = McpServerState::Failed;
        return false;
    }

    std::vector<mcp::tool> tools;
    try {
        tools = client->get_tools();
    } catch (const std::exception& e) {
        LOG_WARN(tag + "get_tools() threw for '" + entry.name + "': " + e.what());
    }

    if (tools.empty()) {
        LOG_INFO(tag + "Server '" + entry.name + "' connected but exposed no tools");
    }

    for (const auto& t : tools) {
        DiscoveredTool dt;
        dt.server_name = entry.name;
        dt.original_tool_name = t.name;
        dt.qualified_name = "mcp_" + sanitize(entry.name) + "_" + t.name;
        dt.definition.name = dt.qualified_name;
        dt.definition.description = t.description;
        dt.definition.parameters = mcp_to_std(t.parameters_schema);
        discovered_tools_.push_back(dt);

        ToolImpl impl;
        impl.definition = dt.definition;
        impl.is_read_only = false;
        impl.source = ToolSource::Mcp;
        const std::string server_name = dt.server_name;
        const std::string tool_name = dt.original_tool_name;
        impl.execute = [this, server_name, tool_name](const std::string& args_json) {
            return invoke(server_name, tool_name, args_json);
        };
        executor.register_tool(impl);
    }

    entry.client = std::move(client);
    entry.state = McpServerState::Connected;
    LOG_INFO(tag + "Server '" + entry.name + "' connected with " +
             std::to_string(tools.size()) + " tool(s)");
    return true;
}

bool McpManager::connect_all(const AppConfig& cfg) {
    if (cfg.mcp_servers.empty()) {
        LOG_INFO("[mcp] No MCP servers configured, skipping connection phase");
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [name, srv_cfg] : cfg.mcp_servers) {
        ServerEntry entry;
        entry.name = name;
        entry.cfg = srv_cfg;
        entry.command_line = build_locator(srv_cfg);
        entry.state = McpServerState::Failed; // upgraded by connect_entry_locked on success
        servers_.push_back(std::move(entry));
    }

    // Connection requires running the same path as enable/reconnect, but
    // connect_all is called before tools are wired. We deliberately defer
    // tool registration until register_tools(executor) is invoked, matching
    // the existing main.cpp two-phase startup. So we only construct entries
    // here; actual connection happens in register_tools.
    return !servers_.empty();
}

void McpManager::register_tools(ToolExecutor& executor) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& entry : servers_) {
        // Only attempt to connect entries we haven't already brought up.
        if (entry.state == McpServerState::Connected && entry.client) {
            continue;
        }
        connect_entry_locked(entry, executor);
    }
}

void McpManager::teardown_locked(ServerEntry& entry, ToolExecutor& executor) {
    if (entry.client) {
        const std::string tag = std::string("[mcp:") + transport_tag(entry.cfg.transport) + "] ";
        LOG_INFO(tag + "Tearing down server '" + entry.name + "'");
        // stdio_client destructor force-kills the child; sse_client destructor
        // stops the SSE reader thread and closes the HTTP connection.
        entry.client.reset();
    }
    // Drop tools owned by this server from both our cache and the executor.
    auto it = discovered_tools_.begin();
    while (it != discovered_tools_.end()) {
        if (it->server_name == entry.name) {
            executor.unregister_tool(it->qualified_name);
            it = discovered_tools_.erase(it);
        } else {
            ++it;
        }
    }
}

bool McpManager::disable(const std::string& name, ToolExecutor& executor) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* entry = find_entry_locked(name);
    if (!entry) return false;
    if (entry->state == McpServerState::Disabled) return false;
    teardown_locked(*entry, executor);
    entry->state = McpServerState::Disabled;
    return true;
}

bool McpManager::enable(const std::string& name, ToolExecutor& executor) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* entry = find_entry_locked(name);
    if (!entry) return false;
    if (entry->state == McpServerState::Connected && entry->client) {
        return false;
    }
    return connect_entry_locked(*entry, executor);
}

bool McpManager::reconnect(const std::string& name, ToolExecutor& executor) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* entry = find_entry_locked(name);
    if (!entry) return false;
    teardown_locked(*entry, executor);
    return connect_entry_locked(*entry, executor);
}

std::vector<McpServerInfo> McpManager::list_servers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<McpServerInfo> out;
    out.reserve(servers_.size());
    for (const auto& e : servers_) {
        size_t tc = 0;
        for (const auto& dt : discovered_tools_) {
            if (dt.server_name == e.name) ++tc;
        }
        out.push_back({e.name, e.state, tc, transport_tag(e.cfg.transport), e.command_line});
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<ToolDef>>>
McpManager::list_tools_by_server() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, std::vector<ToolDef>>> out;
    out.reserve(servers_.size());
    for (const auto& e : servers_) {
        std::vector<ToolDef> defs;
        for (const auto& dt : discovered_tools_) {
            if (dt.server_name == e.name) defs.push_back(dt.definition);
        }
        out.emplace_back(e.name, std::move(defs));
    }
    return out;
}

bool McpManager::has_server(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& e : servers_) {
        if (e.name == name) return true;
    }
    return false;
}

std::vector<std::string> McpManager::server_names() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(servers_.size());
    for (const auto& e : servers_) out.push_back(e.name);
    return out;
}

size_t McpManager::connected_server_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& e : servers_) {
        if (e.state == McpServerState::Connected) ++n;
    }
    return n;
}

ToolResult McpManager::invoke(const std::string& server_name,
                              const std::string& tool_name,
                              const std::string& arguments_json) {
    std::shared_ptr<mcp::client> client;
    std::string tag = "[mcp] ";
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& s : servers_) {
            if (s.name == server_name) {
                client = s.client;
                tag = std::string("[mcp:") + transport_tag(s.cfg.transport) + "] ";
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
            if (out.empty()) out = result.dump();
        } else {
            out = result.dump();
        }
        return ToolResult{out, !is_error};
    } catch (const mcp::mcp_exception& e) {
        LOG_ERROR(tag + "call_tool('" + server_name + "', '" + tool_name + "') mcp_exception: " + e.what());
        return ToolResult{std::string("[Error] MCP call failed: ") + e.what(), false};
    } catch (const std::exception& e) {
        LOG_ERROR(tag + "call_tool('" + server_name + "', '" + tool_name + "') exception: " + e.what());
        return ToolResult{std::string("[Error] MCP call failed: ") + e.what(), false};
    }
}

void McpManager::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    if (shutdown_done_) return;
    shutdown_done_ = true;
    for (auto& s : servers_) {
        const std::string tag = std::string("[mcp:") + transport_tag(s.cfg.transport) + "] ";
        try {
            if (s.client) {
                LOG_INFO(tag + "Shutting down server '" + s.name + "'");
                s.client.reset();
            }
        } catch (const std::exception& e) {
            LOG_WARN(tag + "Exception while killing server '" + s.name + "': " + e.what());
        } catch (...) {
            LOG_WARN(tag + "Unknown exception while killing server '" + s.name + "'");
        }
    }
    servers_.clear();
    discovered_tools_.clear();
}

std::vector<ToolDef> McpManager::get_tool_definitions() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ToolDef> defs;
    defs.reserve(discovered_tools_.size());
    for (const auto& dt : discovered_tools_) {
        defs.push_back(dt.definition);
    }
    return defs;
}

} // namespace acecode
