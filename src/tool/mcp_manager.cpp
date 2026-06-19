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
#include <chrono>
#include <exception>
#include <sstream>
#include <thread>
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

std::string extension_for_mcp_mime(const std::string& mime) {
    std::string lower = mime;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower == "image/jpeg" || lower == "image/jpg") return ".jpg";
    if (lower == "image/gif") return ".gif";
    if (lower == "image/webp") return ".webp";
    if (lower == "image/bmp") return ".bmp";
    return ".png";
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

void configure_cpp_mcp_logger() {
    // cpp-mcp logs info-level process lifecycle messages directly to stderr.
    // During TUI startup those bytes corrupt the already-rendered input area.
    // ACECode emits its own MCP status/log messages, so keep the vendored logger
    // quiet for normal lifecycle chatter.
    mcp::set_log_level(mcp::log_level::error);
}

McpServerState failure_state_for_message(const std::string& message) {
    std::string lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lower.find("timeout") != std::string::npos ||
        lower.find("timed out") != std::string::npos) {
        return McpServerState::TimedOut;
    }
    return McpServerState::Failed;
}

} // namespace

struct McpManager::State {
    mutable std::mutex mu;
    mutable std::condition_variable cv;
    std::vector<ServerEntry> servers;
    std::vector<DiscoveredTool> discovered_tools;
    bool shutdown_done = false;
    StatusCallback status_callback;
};

McpManager::McpManager()
    : state_(std::make_shared<State>()) {
    configure_cpp_mcp_logger();
}

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

void McpManager::set_status_callback(StatusCallback callback) {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    state->status_callback = std::move(callback);
}

McpManager::ServerEntry* McpManager::find_entry_locked(const std::string& name) {
    for (auto& e : state_->servers) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

McpManager::ConnectionSnapshot McpManager::snapshot_for_start_locked(ServerEntry& entry) {
    entry.generation++;
    entry.state = McpServerState::Starting;
    entry.error.clear();
    entry.client.reset();
    return ConnectionSnapshot{entry.name, entry.cfg, entry.command_line, entry.generation};
}

McpManager::ConnectionResult McpManager::connect_entry(ConnectionSnapshot snapshot) {
    ConnectionResult result;
    result.server_name = snapshot.server_name;
    result.generation = snapshot.generation;

    const std::string tag = std::string("[mcp:") + transport_tag(snapshot.cfg.transport) + "] ";
    LOG_INFO(tag + "Connecting to server '" + snapshot.server_name + "': " + snapshot.command_line);

    try {
        if (snapshot.cfg.transport == McpTransport::Stdio) {
            result.client = std::make_shared<mcp::stdio_client>(
                snapshot.command_line,
                env_map_to_mcp_json(snapshot.cfg.env));
        } else if (snapshot.cfg.transport == McpTransport::Sse) {
            auto sse = std::make_shared<mcp::sse_client>(
                snapshot.cfg.url,
                snapshot.cfg.sse_endpoint);
            for (const auto& [k, v] : snapshot.cfg.headers) {
                sse->set_header(k, v);
            }
            if (!snapshot.cfg.auth_token.empty()) {
                sse->set_auth_token(snapshot.cfg.auth_token);
            }
            if (snapshot.cfg.timeout_seconds > 0) {
                sse->set_timeout(snapshot.cfg.timeout_seconds);
            }
            const std::string auth_state = snapshot.cfg.auth_token.empty() ? "none" : "present";
            std::string hdr_keys = header_keys_summary(snapshot.cfg.headers);
            LOG_INFO(tag + "sse config for '" + snapshot.server_name +
                     "' auth=" + auth_state +
                     (hdr_keys.empty() ? "" : (" header_keys=" + hdr_keys)));
            result.client = std::move(sse);
        } else {
            std::string ep = snapshot.cfg.sse_endpoint;
            if (ep.empty() || ep == "/sse") {
                ep = "/mcp";
            }
            auto h = std::make_shared<mcp::streamable_http_client>(
                snapshot.cfg.url,
                ep);
            for (const auto& [k, v] : snapshot.cfg.headers) {
                h->set_header(k, v);
            }
            if (!snapshot.cfg.auth_token.empty()) {
                h->set_auth_token(snapshot.cfg.auth_token);
            }
            if (snapshot.cfg.timeout_seconds > 0) {
                h->set_timeout(snapshot.cfg.timeout_seconds);
            }
            const std::string auth_state = snapshot.cfg.auth_token.empty() ? "none" : "present";
            std::string hdr_keys = header_keys_summary(snapshot.cfg.headers);
            LOG_INFO(tag + "http (streamable) config for '" + snapshot.server_name +
                     "' endpoint=" + ep +
                     " auth=" + auth_state +
                     (hdr_keys.empty() ? "" : (" header_keys=" + hdr_keys)));
            result.client = std::move(h);
        }
    } catch (const std::exception& e) {
        result.error = e.what();
        result.state = failure_state_for_message(result.error);
        LOG_ERROR(tag + "Failed to construct client for '" + snapshot.server_name + "': " + result.error);
        return result;
    }

    bool initialized = false;
    try {
        initialized = result.client->initialize("acecode", "0.1.1");
    } catch (const std::exception& e) {
        result.error = e.what();
        result.state = failure_state_for_message(result.error);
        LOG_ERROR(tag + "initialize() threw for '" + snapshot.server_name + "': " + result.error);
        return result;
    }

    if (!initialized) {
        result.error = "initialization failed";
        result.state = McpServerState::Failed;
        LOG_WARN(tag + "Skipping server '" + snapshot.server_name + "' (initialization failed)");
        return result;
    }

    std::vector<mcp::tool> tools;
    try {
        tools = result.client->get_tools();
    } catch (const std::exception& e) {
        result.error = e.what();
        result.state = failure_state_for_message(result.error);
        LOG_WARN(tag + "get_tools() threw for '" + snapshot.server_name + "': " + result.error);
        return result;
    }

    if (tools.empty()) {
        LOG_INFO(tag + "Server '" + snapshot.server_name + "' connected but exposed no tools");
    }

    result.tools.reserve(tools.size());
    for (const auto& t : tools) {
        DiscoveredTool dt;
        dt.server_name = snapshot.server_name;
        dt.original_tool_name = t.name;
        dt.qualified_name = "mcp_" + sanitize(snapshot.server_name) + "_" + t.name;
        dt.definition.name = dt.qualified_name;
        dt.definition.description = t.description;
        dt.definition.parameters = mcp_to_std(t.parameters_schema);
        result.tools.push_back(std::move(dt));
    }

    result.state = McpServerState::Connected;
    LOG_INFO(tag + "Server '" + snapshot.server_name + "' connected with " +
             std::to_string(tools.size()) + " tool(s)");
    return result;
}

void McpManager::publish_connection_result(const std::shared_ptr<State>& state,
                                           ConnectionResult result,
                                           ToolExecutor& executor) {
    StatusCallback callback;
    McpServerInfo info;
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (state->shutdown_done) {
            return;
        }

        auto it = std::find_if(state->servers.begin(), state->servers.end(), [&](const ServerEntry& entry) {
            return entry.name == result.server_name;
        });
        if (it == state->servers.end() || it->generation != result.generation ||
            it->state != McpServerState::Starting) {
            return;
        }

        auto dt = state->discovered_tools.begin();
        while (dt != state->discovered_tools.end()) {
            if (dt->server_name == it->name) {
                executor.unregister_tool(dt->qualified_name);
                dt = state->discovered_tools.erase(dt);
            } else {
                ++dt;
            }
        }

        it->client.reset();
        it->state = result.state;
        it->error = result.state == McpServerState::Connected ? std::string{} : result.error;

        if (result.state == McpServerState::Connected) {
            it->client = std::move(result.client);
            std::weak_ptr<State> weak_state = state;
            for (const auto& tool : result.tools) {
                ToolImpl impl;
                impl.definition = tool.definition;
                impl.is_read_only = false;
                impl.source = ToolSource::Mcp;
                const std::string server_name = tool.server_name;
                const std::string tool_name = tool.original_tool_name;
                impl.execute = [weak_state, server_name, tool_name](
                                   const std::string& args_json,
                                   const ToolContext& /*ctx*/) {
                    return McpManager::invoke(weak_state, server_name, tool_name, args_json);
                };
                executor.register_tool(impl);
                state->discovered_tools.push_back(tool);
            }
        }

        size_t tool_count = 0;
        for (const auto& tool : state->discovered_tools) {
            if (tool.server_name == it->name) ++tool_count;
        }
        info = McpServerInfo{
            it->name,
            it->state,
            tool_count,
            transport_tag(it->cfg.transport),
            it->command_line,
            it->error,
        };
        callback = state->status_callback;
        should_notify = true;
        state->cv.notify_all();
    }
    if (should_notify && callback) {
        callback(info);
    }
}

void McpManager::start_entry_async(ConnectionSnapshot snapshot, ToolExecutor& executor) {
    auto state = state_;
    std::thread([state, snapshot = std::move(snapshot), &executor]() mutable {
        ConnectionResult result = connect_entry(std::move(snapshot));
        publish_connection_result(state, std::move(result), executor);
    }).detach();
}

bool McpManager::connect_all(const AppConfig& cfg) {
    if (cfg.mcp_servers.empty()) {
        LOG_INFO("[mcp] No MCP servers configured, skipping connection phase");
        return false;
    }

    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    if (!state->servers.empty()) {
        return true;
    }
    for (const auto& [name, srv_cfg] : cfg.mcp_servers) {
        ServerEntry entry;
        entry.name = name;
        entry.cfg = srv_cfg;
        entry.command_line = build_locator(srv_cfg);
        entry.state = McpServerState::Failed;
        state->servers.push_back(std::move(entry));
    }

    return !state->servers.empty();
}

void McpManager::register_tools(ToolExecutor& executor) {
    start_async(executor);
}

void McpManager::start_async(ToolExecutor& executor) {
    std::vector<ConnectionSnapshot> snapshots;
    std::vector<McpServerInfo> updates;
    StatusCallback callback;
    {
        auto state = state_;
        std::lock_guard<std::mutex> lk(state->mu);
        if (state->shutdown_done) {
            return;
        }
        for (auto& entry : state->servers) {
            if (entry.state == McpServerState::Connected ||
                entry.state == McpServerState::Disabled ||
                entry.state == McpServerState::Starting) {
                continue;
            }
            snapshots.push_back(snapshot_for_start_locked(entry));
            updates.push_back(McpServerInfo{
                entry.name,
                entry.state,
                0,
                transport_tag(entry.cfg.transport),
                entry.command_line,
                entry.error,
            });
        }
        callback = state->status_callback;
        state->cv.notify_all();
    }

    if (callback) {
        for (const auto& update : updates) {
            callback(update);
        }
    }
    for (auto& snapshot : snapshots) {
        start_entry_async(std::move(snapshot), executor);
    }
}

void McpManager::teardown_locked(ServerEntry& entry, ToolExecutor& executor) {
    if (entry.client) {
        const std::string tag = std::string("[mcp:") + transport_tag(entry.cfg.transport) + "] ";
        LOG_INFO(tag + "Tearing down server '" + entry.name + "'");
        entry.client.reset();
    }
    auto it = state_->discovered_tools.begin();
    while (it != state_->discovered_tools.end()) {
        if (it->server_name == entry.name) {
            executor.unregister_tool(it->qualified_name);
            it = state_->discovered_tools.erase(it);
        } else {
            ++it;
        }
    }
}

bool McpManager::disable(const std::string& name, ToolExecutor& executor) {
    StatusCallback callback;
    McpServerInfo info;
    {
        auto state = state_;
        std::lock_guard<std::mutex> lk(state->mu);
        auto* entry = find_entry_locked(name);
        if (!entry) return false;
        if (entry->state == McpServerState::Disabled) return false;
        teardown_locked(*entry, executor);
        entry->generation++;
        entry->state = McpServerState::Disabled;
        entry->error.clear();
        info = McpServerInfo{
            entry->name,
            entry->state,
            0,
            transport_tag(entry->cfg.transport),
            entry->command_line,
            entry->error,
        };
        callback = state->status_callback;
        state->cv.notify_all();
    }
    if (callback) {
        callback(info);
    }
    return true;
}

bool McpManager::enable(const std::string& name, ToolExecutor& executor) {
    ConnectionSnapshot snapshot;
    McpServerInfo info;
    StatusCallback callback;
    bool should_start = false;
    {
        auto state = state_;
        std::lock_guard<std::mutex> lk(state->mu);
        auto* entry = find_entry_locked(name);
        if (!entry) return false;
        if (entry->state == McpServerState::Connected || entry->state == McpServerState::Starting) {
            return false;
        }
        snapshot = snapshot_for_start_locked(*entry);
        info = McpServerInfo{
            entry->name,
            entry->state,
            0,
            transport_tag(entry->cfg.transport),
            entry->command_line,
            entry->error,
        };
        callback = state->status_callback;
        should_start = true;
        state->cv.notify_all();
    }
    if (should_start) {
        if (callback) {
            callback(info);
        }
        start_entry_async(std::move(snapshot), executor);
    }
    return should_start;
}

bool McpManager::reconnect(const std::string& name, ToolExecutor& executor) {
    ConnectionSnapshot snapshot;
    McpServerInfo info;
    StatusCallback callback;
    bool should_start = false;
    {
        auto state = state_;
        std::lock_guard<std::mutex> lk(state->mu);
        auto* entry = find_entry_locked(name);
        if (!entry) return false;
        teardown_locked(*entry, executor);
        snapshot = snapshot_for_start_locked(*entry);
        info = McpServerInfo{
            entry->name,
            entry->state,
            0,
            transport_tag(entry->cfg.transport),
            entry->command_line,
            entry->error,
        };
        callback = state->status_callback;
        should_start = true;
        state->cv.notify_all();
    }
    if (should_start) {
        if (callback) {
            callback(info);
        }
        start_entry_async(std::move(snapshot), executor);
    }
    return should_start;
}

std::vector<McpServerInfo> McpManager::list_servers() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    std::vector<McpServerInfo> out;
    out.reserve(state->servers.size());
    for (const auto& e : state->servers) {
        size_t tc = 0;
        for (const auto& dt : state->discovered_tools) {
            if (dt.server_name == e.name) ++tc;
        }
        out.push_back({e.name, e.state, tc, transport_tag(e.cfg.transport), e.command_line, e.error});
    }
    return out;
}

std::vector<std::pair<std::string, std::vector<ToolDef>>>
McpManager::list_tools_by_server() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    std::vector<std::pair<std::string, std::vector<ToolDef>>> out;
    out.reserve(state->servers.size());
    for (const auto& e : state->servers) {
        std::vector<ToolDef> defs;
        for (const auto& dt : state->discovered_tools) {
            if (dt.server_name == e.name) defs.push_back(dt.definition);
        }
        out.emplace_back(e.name, std::move(defs));
    }
    return out;
}

bool McpManager::has_server(const std::string& name) const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    for (const auto& e : state->servers) {
        if (e.name == name) return true;
    }
    return false;
}

std::vector<std::string> McpManager::server_names() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    std::vector<std::string> out;
    out.reserve(state->servers.size());
    for (const auto& e : state->servers) out.push_back(e.name);
    return out;
}

size_t McpManager::connected_server_count() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    size_t n = 0;
    for (const auto& e : state->servers) {
        if (e.state == McpServerState::Connected) ++n;
    }
    return n;
}

size_t McpManager::configured_server_count() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    return state->servers.size();
}

size_t McpManager::discovered_tool_count() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    return state->discovered_tools.size();
}

bool McpManager::has_starting_servers() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    for (const auto& e : state->servers) {
        if (e.state == McpServerState::Starting) return true;
    }
    return false;
}

bool McpManager::wait_for_startup_settled(std::chrono::milliseconds timeout) const {
    auto state = state_;
    std::unique_lock<std::mutex> lk(state->mu);
    return state->cv.wait_for(lk, timeout, [&]() {
        if (state->shutdown_done) return true;
        for (const auto& e : state->servers) {
            if (e.state == McpServerState::Starting) return false;
        }
        return true;
    });
}

ToolResult McpManager::invoke(const std::weak_ptr<State>& weak_state,
                              const std::string& server_name,
                              const std::string& tool_name,
                              const std::string& arguments_json) {
    auto state = weak_state.lock();
    if (!state) {
        return ToolResult{"[Error] MCP manager is no longer available", false};
    }

    std::shared_ptr<mcp::client> client;
    std::string tag = "[mcp] ";
    {
        std::lock_guard<std::mutex> lk(state->mu);
        for (const auto& s : state->servers) {
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
        nlohmann::json attachments = nlohmann::json::array();
        if (result.contains("content") && result["content"].is_array()) {
            std::ostringstream oss;
            int image_index = 1;
            for (const auto& item : result["content"]) {
                if (item.is_object() && item.contains("type") && item["type"] == "text" &&
                    item.contains("text") && item["text"].is_string()) {
                    if (!oss.str().empty()) oss << '\n';
                    oss << item["text"].get<std::string>();
                } else if (item.is_object() && item.contains("type") && item["type"] == "image" &&
                           item.contains("data") && item["data"].is_string()) {
                    const std::string mime = item.value("mimeType", item.value("mime_type", std::string{"image/png"}));
                    std::string name = item.value("name", std::string{});
                    if (name.empty()) {
                        name = "mcp-image-" + std::to_string(image_index) + extension_for_mcp_mime(mime);
                    }
                    attachments.push_back(nlohmann::json{
                        {"name", name},
                        {"mime_type", mime},
                        {"data_url", "data:" + mime + ";base64," + item["data"].get<std::string>()},
                    });
                    image_index++;
                }
            }
            out = oss.str();
            if (out.empty()) out = result.dump();
        } else {
            out = result.dump();
        }
        ToolResult tool_result{out, !is_error};
        if (!attachments.empty()) tool_result.attachments = std::move(attachments);
        return tool_result;
    } catch (const mcp::mcp_exception& e) {
        LOG_ERROR(tag + "call_tool('" + server_name + "', '" + tool_name + "') mcp_exception: " + e.what());
        return ToolResult{std::string("[Error] MCP call failed: ") + e.what(), false};
    } catch (const std::exception& e) {
        LOG_ERROR(tag + "call_tool('" + server_name + "', '" + tool_name + "') exception: " + e.what());
        return ToolResult{std::string("[Error] MCP call failed: ") + e.what(), false};
    }
}

void McpManager::shutdown() {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    if (state->shutdown_done) return;
    state->shutdown_done = true;
    for (auto& s : state->servers) {
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
    state->servers.clear();
    state->discovered_tools.clear();
    state->cv.notify_all();
}

std::vector<ToolDef> McpManager::get_tool_definitions() const {
    auto state = state_;
    std::lock_guard<std::mutex> lk(state->mu);
    std::vector<ToolDef> defs;
    defs.reserve(state->discovered_tools.size());
    for (const auto& dt : state->discovered_tools) {
        defs.push_back(dt.definition);
    }
    return defs;
}

} // namespace acecode
