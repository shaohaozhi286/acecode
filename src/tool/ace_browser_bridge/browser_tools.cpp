#include "browser_tools.hpp"

#include "session/session_manager.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace acecode::ace_browser_bridge {
namespace {

constexpr std::size_t kMaxTextBytes = 16000;
constexpr std::size_t kMaxStringBytes = 8000;
constexpr int kMaxElements = 80;
constexpr int kTypeTextLimit = 4000;

struct BrowserToolState {
    AceBrowserBridgeConfig config;
    std::shared_ptr<AceBrowserBridgeClient> client;
    std::set<std::string> enabled_groups;
    std::mutex mu;
    std::atomic<std::uint64_t> snapshot_counter{0};
};

const std::map<std::string, std::vector<std::string>>& group_tools() {
    static const std::map<std::string, std::vector<std::string>> groups = {
        {"interaction", {"browser_click", "browser_fill", "browser_type"}},
        {"pointer", {"browser_hover", "browser_drag", "browser_scroll"}},
        {"capture", {"browser_screenshot", "browser_save_pdf"}},
        {"network", {"browser_network"}},
        {"diagnostics", {"browser_trace", "browser_list_tabs"}},
        {"advanced", {"browser_evaluate", "browser_upload"}},
    };
    return groups;
}

nlohmann::json envelope_json(const BridgeEnvelope& envelope) {
    nlohmann::json out;
    out["ok"] = envelope.ok;
    if (envelope.ok) {
        out["data"] = envelope.data.is_null() ? nlohmann::json::object() : envelope.data;
    } else {
        out["error"] = {
            {"code", envelope.error ? envelope.error->code : "bridge_error"},
            {"message", envelope.error ? envelope.error->message : "browser bridge command failed"},
        };
    }
    return out;
}

ToolResult envelope_result(const BridgeEnvelope& envelope) {
    return ToolResult{envelope_json(envelope).dump(2), envelope.ok};
}

BridgeEnvelope make_error(std::string code, std::string message) {
    BridgeEnvelope envelope;
    envelope.ok = false;
    envelope.error = BridgeError{std::move(code), std::move(message)};
    return envelope;
}

ToolResult error_result(const std::string& code, const std::string& message) {
    return envelope_result(make_error(code, message));
}

bool parse_args(const std::string& arguments_json, nlohmann::json& args, std::string& error) {
    args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json, nullptr, false);
    if (args.is_discarded() || !args.is_object()) {
        error = "arguments must be a JSON object";
        return false;
    }
    return true;
}

std::string require_string(const nlohmann::json& args, const char* key, std::string& error) {
    if (!args.contains(key) || !args[key].is_string() || args[key].get<std::string>().empty()) {
        error = std::string("missing required string argument: ") + key;
        return {};
    }
    return args[key].get<std::string>();
}

std::string optional_string(const nlohmann::json& args, const char* key) {
    if (args.contains(key) && args[key].is_string()) return args[key].get<std::string>();
    return {};
}

std::string sanitize_session_part(std::string value) {
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '-' && ch != '_') ch = '-';
    }
    while (!value.empty() && value.front() == '-') value.erase(value.begin());
    while (!value.empty() && value.back() == '-') value.pop_back();
    if (value.size() > 48) value.resize(48);
    return value.empty() ? "default" : value;
}

std::string default_session_name(const nlohmann::json& args, const ToolContext& ctx) {
    std::string explicit_session = optional_string(args, "session");
    if (!explicit_session.empty()) return explicit_session;
    if (ctx.session_manager) {
        std::string session_id = ctx.session_manager->current_session_id();
        if (!session_id.empty()) return "acecode-" + sanitize_session_part(session_id);
    }
    return "acecode-default";
}

std::vector<std::string> enabled_groups_snapshot(const std::shared_ptr<BrowserToolState>& state) {
    std::lock_guard<std::mutex> lk(state->mu);
    return std::vector<std::string>(state->enabled_groups.begin(), state->enabled_groups.end());
}

void set_group_enabled(const std::shared_ptr<BrowserToolState>& state, const std::string& group) {
    std::lock_guard<std::mutex> lk(state->mu);
    state->enabled_groups.insert(group);
}

bool is_group_enabled(const std::shared_ptr<BrowserToolState>& state, const std::string& group) {
    std::lock_guard<std::mutex> lk(state->mu);
    return state->enabled_groups.count(group) != 0;
}

void scrub_large_payloads(nlohmann::json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end();) {
            const std::string key = it.key();
            if ((key == "data" || key == "base64") && it->is_string() &&
                it->get<std::string>().size() > 200) {
                it = value.erase(it);
                continue;
            }
            if ((key == "body" || key == "text" || key == "value") && it->is_string()) {
                const std::string s = it->get<std::string>();
                if (s.size() > kMaxStringBytes) {
                    *it = truncate_utf8_prefix(s, kMaxStringBytes) + "\n[truncated]";
                }
            } else {
                scrub_large_payloads(*it);
            }
            ++it;
        }
    } else if (value.is_array()) {
        for (auto& item : value) scrub_large_payloads(item);
    } else if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s.size() > kMaxStringBytes) {
            value = truncate_utf8_prefix(s, kMaxStringBytes) + "\n[truncated]";
        }
    }
}

void append_tree_text(const nlohmann::json& node, std::string& out, int& count) {
    if (count > 400 || out.size() > kMaxTextBytes) return;
    if (node.is_string()) {
        std::string s = node.get<std::string>();
        if (!s.empty()) {
            if (!out.empty()) out.push_back('\n');
            out += s;
            ++count;
        }
        return;
    }
    if (node.is_array()) {
        for (const auto& item : node) append_tree_text(item, out, count);
        return;
    }
    if (!node.is_object()) return;

    std::string line;
    for (const char* key : {"ref", "role", "name", "text", "value"}) {
        if (node.contains(key) && node[key].is_string() && !node[key].get<std::string>().empty()) {
            if (!line.empty()) line += " ";
            line += node[key].get<std::string>();
        }
    }
    if (!line.empty()) {
        if (!out.empty()) out.push_back('\n');
        out += line;
        ++count;
    }
    for (const char* key : {"children", "tree", "items"}) {
        if (node.contains(key)) append_tree_text(node[key], out, count);
    }
}

void collect_elements(const nlohmann::json& node, nlohmann::json& elements, int& count) {
    if (count >= kMaxElements) return;
    if (node.is_array()) {
        for (const auto& item : node) collect_elements(item, elements, count);
        return;
    }
    if (!node.is_object()) return;

    std::string ref = optional_string(node, "ref");
    if (ref.empty() && node.contains("selector") && node["selector"].is_string()) {
        ref = node["selector"].get<std::string>();
    }
    if (!ref.empty()) {
        nlohmann::json item;
        item["ref"] = ref;
        for (const char* key : {"role", "name", "text", "tag"}) {
            if (node.contains(key) && node[key].is_string()) item[key] = node[key];
        }
        if (node.contains("rect")) item["rect"] = node["rect"];
        elements.push_back(std::move(item));
        ++count;
    }

    for (const char* key : {"children", "tree", "items"}) {
        if (node.contains(key)) collect_elements(node[key], elements, count);
    }
}

nlohmann::json compact_snapshot_data(const std::shared_ptr<BrowserToolState>& state,
                                     const nlohmann::json& data) {
    nlohmann::json out;
    out["snapshot_id"] = data.value("snapshot_id",
        "snap_" + std::to_string(++state->snapshot_counter));
    if (data.contains("url")) out["url"] = data["url"];
    if (data.contains("title")) out["title"] = data["title"];

    if (data.contains("text") && data["text"].is_string()) {
        out["text"] = truncate_utf8_prefix(data["text"].get<std::string>(), kMaxTextBytes);
    } else if (data.contains("tree")) {
        std::string text;
        int count = 0;
        append_tree_text(data["tree"], text, count);
        out["text"] = truncate_utf8_prefix(text, kMaxTextBytes);
    }

    nlohmann::json elements = nlohmann::json::array();
    if (data.contains("elements") && data["elements"].is_array()) {
        int count = 0;
        collect_elements(data["elements"], elements, count);
    } else if (data.contains("tree")) {
        int count = 0;
        collect_elements(data["tree"], elements, count);
    }
    out["elements"] = std::move(elements);

    for (const char* key : {"focused", "changes", "active_element"}) {
        if (data.contains(key)) out[key] = data[key];
    }
    scrub_large_payloads(out);
    return out;
}

BridgeEnvelope health_gate(const std::shared_ptr<BrowserToolState>& state) {
    BridgeEnvelope status = state->client->status();
    if (!status.ok) return status;
    if (!status.data.value("running", false)) {
        std::string message = "ace-browser-host daemon is not running";
        if (status.data.contains("auto_start_error") && status.data["auto_start_error"].is_string()) {
            message += "; auto-start failed: " + status.data["auto_start_error"].get<std::string>();
        }
        return make_error("daemon_not_running", message);
    }
    if (!status.data.value("extension_connected", false)) {
        return make_error("extension_not_connected", "ace-browser-bridge extension is not connected");
    }
    return BridgeEnvelope{true, nlohmann::json::object(), std::nullopt};
}

ToolResult command_result(const std::shared_ptr<BrowserToolState>& state,
                          const ToolContext& ctx,
                          const nlohmann::json& args,
                          const std::string& action,
                          nlohmann::json bridge_args,
                          bool compact_snapshot = false) {
    BridgeEnvelope health = health_gate(state);
    if (!health.ok) return envelope_result(health);

    BrowserCommandRequest request;
    request.session = default_session_name(args, ctx);
    request.action = action;
    bridge_args["session"] = request.session;
    request.args = std::move(bridge_args);

    BridgeEnvelope envelope = state->client->command(request);
    if (envelope.ok) {
        if (compact_snapshot) {
            envelope.data = compact_snapshot_data(state, envelope.data);
        } else {
            scrub_large_payloads(envelope.data);
        }
    }
    return envelope_result(envelope);
}

nlohmann::json passthrough_args(const nlohmann::json& args,
                                std::initializer_list<const char*> keys) {
    nlohmann::json out = nlohmann::json::object();
    for (const char* key : keys) {
        if (args.contains(key)) out[key] = args[key];
    }
    return out;
}

nlohmann::json with_selector_arg(const nlohmann::json& args) {
    nlohmann::json out = passthrough_args(args,
        {"mode", "speed", "duration_ms", "hold_ms", "jitter", "button",
         "snapshot_id", "relative_position", "debug_visualization", "debug_duration_ms"});
    if (args.contains("target")) out["selector"] = args["target"];
    if (args.contains("x")) out["x"] = args["x"];
    if (args.contains("y")) out["y"] = args["y"];
    return out;
}

void apply_interaction_defaults(const std::shared_ptr<BrowserToolState>& state,
                                const nlohmann::json& args,
                                nlohmann::json& bridge_args,
                                bool include_speed) {
    if (!bridge_args.contains("mode")) {
        bridge_args["mode"] = args.value("mode", state->config.default_mode);
    }
    if (include_speed && !bridge_args.contains("speed")) {
        bridge_args["speed"] = args.value("speed", state->config.pointer_speed);
    }
    if (include_speed && bridge_args.value("speed", "") == "custom") {
        const auto& pc = state->config.pointer_custom;
        bridge_args["pointer_custom"] = {
            {"move_duration_ms_min", pc.move_duration_ms_min},
            {"move_duration_ms_max", pc.move_duration_ms_max},
            {"click_hold_ms_min", pc.click_hold_ms_min},
            {"click_hold_ms_max", pc.click_hold_ms_max},
            {"typing_delay_ms_min", pc.typing_delay_ms_min},
            {"typing_delay_ms_max", pc.typing_delay_ms_max},
            {"jitter_px", pc.jitter_px},
            {"max_path_points", pc.max_path_points},
        };
    }
}

nlohmann::json object_schema(nlohmann::json properties,
                             nlohmann::json required = nlohmann::json::array()) {
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
        {"additionalProperties", false},
    };
}

nlohmann::json string_prop(const std::string& description) {
    return {{"type", "string"}, {"description", description}};
}

nlohmann::json session_prop() {
    return string_prop("Browser session name. Defaults to the current ACECode thread session.");
}

nlohmann::json mode_prop() {
    return {
        {"type", "string"},
        {"enum", {"auto", "dom", "cdp", "os"}},
        {"description", "Interaction transport. auto prefers CDP pointer actions and reports actual mode plus fallback reason when fallback happens."},
    };
}

nlohmann::json speed_prop() {
    return {
        {"type", "string"},
        {"enum", {"fast", "normal", "slow", "custom"}},
        {"description", "Pointer speed override for this action."},
    };
}

ToolImpl make_status_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_status";
    def.description =
        "Report ace-browser-host daemon, ace-browser-bridge extension, host versions, capabilities, and enabled browser tool groups.";
    def.parameters = object_schema(nlohmann::json::object());

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [state](const std::string&, const ToolContext&) {
        BridgeEnvelope envelope = state->client->status();
        if (envelope.ok) {
            if (!envelope.data.contains("capabilities") || !envelope.data["capabilities"].is_object()) {
                envelope.data["capabilities"] = nlohmann::json::object();
            }
            auto& cap = envelope.data["capabilities"];
            if (!cap.contains("cdp")) cap["cdp"] = false;
            if (!cap.contains("network")) cap["network"] = false;
            if (!cap.contains("pdf")) cap["pdf"] = false;
            if (!cap.contains("upload")) cap["upload"] = false;
            cap["os_pointer"] = state->config.os_pointer_enabled;
            cap["operation_overlay"] = state->config.operation_overlay_enabled;
            envelope.data["enabled_tool_groups"] = enabled_groups_snapshot(state);
            envelope.data["tool_mode"] = state->config.tool_mode;
        }
        return envelope_result(envelope);
    };
    return impl;
}

ToolImpl make_enable_tool(const std::shared_ptr<BrowserToolState>& state);
void register_tool_group(ToolExecutor& tools,
                         const std::shared_ptr<BrowserToolState>& state,
                         const std::string& group);

ToolImpl make_open_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_open";
    def.description =
        "Open a URL in the managed browser. New tabs created by this tool are reported as owned by the browser session.";
    def.parameters = object_schema({
        {"url", string_prop("URL to open.")},
        {"session", session_prop()},
        {"new_tab", {{"type", "boolean"}, {"description", "Open in a new tab. Defaults to false; omitted calls reuse the session tab when possible."}}},
        {"group_title", string_prop("Optional browser tab group title.")},
    }, {"url"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string url = require_string(args, "url", error);
        if (!error.empty()) return error_result("invalid_arguments", error);
        nlohmann::json bridge_args;
        bridge_args["url"] = url;
        bridge_args["newTab"] = args.value("new_tab", false);
        if (args.contains("group_title")) bridge_args["group_title"] = args["group_title"];
        ToolResult result = command_result(state, ctx, args, "navigate", std::move(bridge_args));
        if (result.success) {
            auto j = nlohmann::json::parse(result.output, nullptr, false);
            if (j.is_object() && j.contains("data")) {
                if (!j["data"].contains("ownership")) j["data"]["ownership"] = "owned";
                result.output = j.dump(2);
            }
        }
        return result;
    };
    return impl;
}

ToolImpl make_find_tab_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_find_tab";
    def.description =
        "Find or adopt an existing browser tab by URL/domain, tab id, or the active user tab.";
    def.parameters = object_schema({
        {"url", string_prop("URL or domain substring to match.")},
        {"tab_id", {{"type", "integer"}, {"description", "Exact browser tab id to adopt."}}},
        {"session", session_prop()},
        {"active", {{"type", "boolean"}, {"description", "Use the active user tab."}}},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        if (!args.value("active", false) && optional_string(args, "url").empty() && !args.contains("tab_id")) {
            return error_result("invalid_arguments", "browser_find_tab requires url, tab_id, or active=true");
        }
        nlohmann::json bridge_args = passthrough_args(args, {"url", "active", "tab_id"});
        ToolResult result = command_result(state, ctx, args, "find_tab", std::move(bridge_args));
        if (result.success) {
            auto j = nlohmann::json::parse(result.output, nullptr, false);
            if (j.is_object() && j.contains("data")) {
                j["data"]["ownership"] = "adopted";
                result.output = j.dump(2);
            }
        }
        return result;
    };
    return impl;
}

ToolImpl make_navigate_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_navigate";
    def.description = "Navigate the current managed browser tab: goto, back, forward, or reload.";
    def.parameters = object_schema({
        {"operation", {
            {"type", "string"},
            {"enum", {"goto", "back", "forward", "reload"}},
            {"description", "Navigation operation."},
        }},
        {"url", string_prop("Required for operation=goto.")},
        {"session", session_prop()},
    }, {"operation"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string op = require_string(args, "operation", error);
        if (!error.empty()) return error_result("invalid_arguments", error);
        if (op != "goto" && op != "back" && op != "forward" && op != "reload") {
            return error_result("invalid_arguments", "operation must be one of goto, back, forward, reload");
        }
        nlohmann::json bridge_args;
        if (op == "goto") {
            std::string url = require_string(args, "url", error);
            if (!error.empty()) return error_result("invalid_arguments", "operation=goto requires url");
            bridge_args["url"] = url;
            bridge_args["newTab"] = false;
        } else {
            bridge_args["operation"] = op;
        }
        return command_result(state, ctx, args, "navigate", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_read_page_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_read_page";
    def.description =
        "Read the current page snapshot. Use returned @e refs for click, fill, hover, drag, and type; CSS selectors are fallback targets.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"mode", {
            {"type", "string"},
            {"enum", {"summary", "elements", "focused", "changed"}},
            {"description", "Read mode. Defaults to summary."},
        }},
        {"since_snapshot_id", string_prop("Previous snapshot id for changed mode.")},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string mode = args.value("mode", "summary");
        if (mode != "summary" && mode != "elements" && mode != "focused" && mode != "changed") {
            return error_result("invalid_arguments", "mode must be summary, elements, focused, or changed");
        }
        if (mode == "changed" && optional_string(args, "since_snapshot_id").empty()) {
            return error_result("invalid_arguments", "changed mode requires since_snapshot_id");
        }
        nlohmann::json bridge_args = passthrough_args(args, {"mode", "since_snapshot_id"});
        return command_result(state, ctx, args, "snapshot", std::move(bridge_args), true);
    };
    return impl;
}

ToolImpl make_wait_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_wait";
    def.description =
        "Wait for a browser condition such as URL, text, element state, network idle, or request completion.";
    def.parameters = object_schema({
        {"condition", {
            {"type", "string"},
            {"enum", {"url_contains", "url_matches", "text_present", "element_present",
                      "element_visible", "element_clickable", "network_idle", "request_finished"}},
            {"description", "Condition to wait for."},
        }},
        {"session", session_prop()},
        {"target", string_prop("Element target for element conditions.")},
        {"text", string_prop("Text for text_present.")},
        {"url", string_prop("URL fragment or pattern for URL conditions.")},
        {"request_id", string_prop("Request id for request_finished.")},
        {"timeout_ms", {{"type", "integer"}, {"minimum", 1}, {"description", "Maximum wait time."}}},
    }, {"condition"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string condition = require_string(args, "condition", error);
        if (!error.empty()) return error_result("invalid_arguments", error);
        if ((condition.rfind("element_", 0) == 0) && optional_string(args, "target").empty()) {
            return error_result("invalid_arguments", condition + " requires target");
        }
        if (condition == "text_present" && optional_string(args, "text").empty()) {
            return error_result("invalid_arguments", "text_present requires text");
        }
        if ((condition == "url_contains" || condition == "url_matches") &&
            optional_string(args, "url").empty()) {
            return error_result("invalid_arguments", condition + " requires url");
        }
        if (condition == "request_finished" && optional_string(args, "request_id").empty()) {
            return error_result("invalid_arguments", "request_finished requires request_id");
        }
        return command_result(state, ctx, args, "wait",
                              passthrough_args(args, {"condition", "target", "text", "url",
                                                      "request_id", "timeout_ms"}));
    };
    return impl;
}

ToolImpl make_close_session_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_close_session";
    def.description = "Close or detach tabs associated with a browser session.";
    def.parameters = object_schema({
        {"session", session_prop()},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        return command_result(state, ctx, args, "close_session", nlohmann::json::object());
    };
    return impl;
}

ToolImpl make_click_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_click";
    def.description =
        "Click a browser target by @e ref, CSS selector, or coordinates. Prefer @e refs returned by browser_read_page. Pointer results report actual mode, speed, and fallback reason when fallback happens.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"target", string_prop("@e ref or CSS selector.")},
        {"x", {{"type", "number"}, {"description", "Viewport x coordinate."}}},
        {"y", {{"type", "number"}, {"description", "Viewport y coordinate."}}},
        {"mode", mode_prop()},
        {"speed", speed_prop()},
        {"duration_ms", {{"type", "integer"}, {"minimum", 0}}},
        {"hold_ms", {{"type", "integer"}, {"minimum", 0}}},
        {"jitter", {{"type", "number"}, {"minimum", 0}}},
        {"debug_visualization", {{"type", "boolean"}, {"description", "Temporarily draw the pointer path for this action."}}},
        {"debug_duration_ms", {{"type", "integer"}, {"minimum", 500}, {"maximum", 15000}}},
        {"button", {{"type", "string"}, {"enum", {"left", "middle", "right"}}}},
        {"snapshot_id", string_prop("Snapshot id associated with an @e ref.")},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        const bool has_target = !optional_string(args, "target").empty();
        const bool has_xy = args.contains("x") && args.contains("y") &&
                            args["x"].is_number() && args["y"].is_number();
        if (!has_target && !has_xy) return error_result("invalid_arguments", "browser_click requires target or x/y");
        if (args.value("mode", state->config.default_mode) == "os" && !state->config.os_pointer_enabled) {
            return error_result("os_pointer_disabled", "OS pointer mode is disabled in ace_browser_bridge config");
        }
        nlohmann::json bridge_args = with_selector_arg(args);
        apply_interaction_defaults(state, args, bridge_args, true);
        return command_result(state, ctx, args, "click", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_fill_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_fill";
    def.description = "Replace text in an input, textarea, or contenteditable target.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"target", string_prop("@e ref or CSS selector.")},
        {"value", string_prop("Replacement text.")},
        {"mode", mode_prop()},
        {"snapshot_id", string_prop("Snapshot id associated with an @e ref.")},
    }, {"target", "value"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string target = require_string(args, "target", error);
        std::string value = require_string(args, "value", error);
        if (!error.empty()) return error_result("invalid_arguments", error);
        nlohmann::json bridge_args = passthrough_args(args, {"mode", "snapshot_id"});
        apply_interaction_defaults(state, args, bridge_args, false);
        bridge_args["selector"] = target;
        bridge_args["value"] = value;
        return command_result(state, ctx, args, "fill", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_type_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_type";
    def.description =
        "Focus a target and type text or special keys with bounded delays. Reports actual mode and fallback reason when fallback happens. Use browser_fill for long text replacement.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"target", string_prop("@e ref or CSS selector.")},
        {"text", string_prop("Text to type.")},
        {"clear", {{"type", "boolean"}, {"description", "Clear the target before typing."}}},
        {"submit", {{"type", "boolean"}, {"description", "Send Enter after typing."}}},
        {"keys", {{"type", "array"}, {"items", {{"type", "string"}}}}},
        {"delay_ms", {{"type", "array"}, {"items", {{"type", "integer"}}},
                       {"minItems", 2}, {"maxItems", 2}}},
        {"mode", mode_prop()},
        {"speed", speed_prop()},
    }, {"target"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string target = require_string(args, "target", error);
        if (!error.empty()) return error_result("invalid_arguments", error);
        std::string text = optional_string(args, "text");
        if (text.size() > kTypeTextLimit) {
            return error_result("text_too_long_for_type", "browser_type text is too long; use browser_fill for long text");
        }
        nlohmann::json bridge_args = passthrough_args(args,
            {"text", "clear", "submit", "keys", "delay_ms", "mode", "speed"});
        apply_interaction_defaults(state, args, bridge_args, true);
        bridge_args["selector"] = target;
        return command_result(state, ctx, args, "type", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_hover_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_hover";
    def.description = "Move the pointer to a browser target without pressing a button. Result reports actual mode, speed, path summary, and fallback reason when fallback happens.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"target", string_prop("@e ref or CSS selector.")},
        {"mode", mode_prop()},
        {"speed", speed_prop()},
        {"duration_ms", {{"type", "integer"}, {"minimum", 0}}},
        {"debug_visualization", {{"type", "boolean"}, {"description", "Temporarily draw the pointer path for this action."}}},
        {"debug_duration_ms", {{"type", "integer"}, {"minimum", 500}, {"maximum", 15000}}},
        {"snapshot_id", string_prop("Snapshot id associated with an @e ref.")},
    }, {"target"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        if (require_string(args, "target", error).empty()) return error_result("invalid_arguments", error);
        nlohmann::json bridge_args = with_selector_arg(args);
        apply_interaction_defaults(state, args, bridge_args, true);
        return command_result(state, ctx, args, "hover", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_drag_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_drag";
    def.description = "Drag from one browser target to another target or by an offset. Result reports actual mode, speed, path summary, and fallback reason when fallback happens.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"from", string_prop("Start target @e ref or CSS selector.")},
        {"to", string_prop("Destination target @e ref or CSS selector.")},
        {"offset", {{"type", "array"}, {"items", {{"type", "number"}}},
                     {"minItems", 2}, {"maxItems", 2}}},
        {"mode", mode_prop()},
        {"speed", speed_prop()},
        {"duration_ms", {{"type", "integer"}, {"minimum", 0}}},
        {"hold_ms", {{"type", "integer"}, {"minimum", 0}}},
        {"debug_visualization", {{"type", "boolean"}, {"description", "Temporarily draw the pointer path for this action."}}},
        {"debug_duration_ms", {{"type", "integer"}, {"minimum", 500}, {"maximum", 15000}}},
    }, {"from"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        if (require_string(args, "from", error).empty()) return error_result("invalid_arguments", error);
        if (optional_string(args, "to").empty() && !args.contains("offset")) {
            return error_result("invalid_arguments", "browser_drag requires to or offset");
        }
        nlohmann::json bridge_args = passthrough_args(args, {"from", "to", "offset", "mode",
                                                             "speed", "duration_ms", "hold_ms",
                                                             "debug_visualization", "debug_duration_ms"});
        apply_interaction_defaults(state, args, bridge_args, true);
        return command_result(state, ctx, args, "drag", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_scroll_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_scroll";
    def.description = "Scroll the current page or a target area with wheel deltas. Pointer results report actual mode and fallback reason when fallback happens.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"target", string_prop("Optional @e ref or CSS selector.")},
        {"delta_x", {{"type", "number"}}},
        {"delta_y", {{"type", "number"}}},
        {"mode", mode_prop()},
        {"speed", speed_prop()},
    }, {"delta_y"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        nlohmann::json bridge_args = passthrough_args(args, {"delta_x", "delta_y", "mode", "speed"});
        if (args.contains("target")) bridge_args["selector"] = args["target"];
        apply_interaction_defaults(state, args, bridge_args, true);
        return command_result(state, ctx, args, "scroll", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_network_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_network";
    def.description = "Start, stop, list, or inspect browser network capture for a session.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"cmd", {{"type", "string"}, {"enum", {"start", "stop", "list", "detail"}}}},
        {"filter", string_prop("Filter string for list.")},
        {"request_id", string_prop("Request id for detail.")},
    }, {"cmd"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string cmd = require_string(args, "cmd", error);
        if (!error.empty()) return error_result("invalid_arguments", error);
        if (cmd != "start" && cmd != "stop" && cmd != "list" && cmd != "detail") {
            return error_result("invalid_arguments", "cmd must be start, stop, list, or detail");
        }
        if (cmd == "detail" && optional_string(args, "request_id").empty()) {
            return error_result("invalid_arguments", "browser_network detail requires request_id");
        }
        nlohmann::json bridge_args;
        bridge_args["cmd"] = cmd;
        if (args.contains("filter")) bridge_args["filter"] = args["filter"];
        if (args.contains("request_id")) bridge_args["requestId"] = args["request_id"];
        return command_result(state, ctx, args, "network", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_screenshot_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_screenshot";
    def.description = "Save a browser screenshot to disk and return file metadata only.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"output_path", string_prop("Local output file path. Defaults to a temp PNG path.")},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        BridgeEnvelope health = health_gate(state);
        if (!health.ok) return envelope_result(health);

        std::string output = optional_string(args, "output_path");
        if (output.empty()) {
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::filesystem::path path =
                std::filesystem::temp_directory_path() /
                ("ace-browser-screenshot-" + std::to_string(now) + ".png");
            output = path_to_utf8(path);
        }
        BridgeEnvelope envelope = state->client->screenshot(default_session_name(args, ctx), output);
        if (envelope.ok) scrub_large_payloads(envelope.data);
        return envelope_result(envelope);
    };
    return impl;
}

ToolImpl make_save_pdf_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_save_pdf";
    def.description = "Save the current browser page as PDF and return local file metadata.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"paper_format", string_prop("Paper format, for example a4 or letter.")},
        {"landscape", {{"type", "boolean"}}},
        {"scale", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 2.0}}},
        {"print_background", {{"type", "boolean"}}},
        {"file_name", string_prop("Output PDF file name.")},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        nlohmann::json bridge_args = passthrough_args(args, {"paper_format", "landscape",
                                                             "scale", "print_background",
                                                             "file_name"});
        return command_result(state, ctx, args, "save_as_pdf", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_trace_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_trace";
    def.description = "Return recent browser operation trace summaries for diagnostics.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        return command_result(state, ctx, args, "trace", passthrough_args(args, {"limit"}));
    };
    return impl;
}

ToolImpl make_list_tabs_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_list_tabs";
    def.description = "List tabs known to the browser bridge with session, ownership, and tab group metadata when available.";
    def.parameters = object_schema({
        {"session", session_prop()},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        return command_result(state, ctx, args, "list_tabs", nlohmann::json::object());
    };
    return impl;
}

ToolImpl make_evaluate_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_evaluate";
    def.description =
        "Run a small page-context JavaScript snippet for DOM inspection or targeted page interaction. Prefer compact JSON output.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"code", string_prop("JavaScript code to execute in the page context.")},
    }, {"code"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string code = require_string(args, "code", error);
        if (!error.empty()) return error_result("invalid_arguments", error);
        return command_result(state, ctx, args, "evaluate", nlohmann::json{{"code", code}});
    };
    return impl;
}

ToolImpl make_upload_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_upload";
    def.description = "Upload explicit local files to a browser file input target.";
    def.parameters = object_schema({
        {"session", session_prop()},
        {"target", string_prop("@e ref or CSS selector for file input.")},
        {"files", {{"type", "array"}, {"items", {{"type", "string"}}}}},
    }, {"target", "files"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        std::string target = require_string(args, "target", error);
        if (!args.contains("files") || !args["files"].is_array() || args["files"].empty()) {
            return error_result("invalid_arguments", "browser_upload requires a non-empty files array");
        }
        nlohmann::json files = nlohmann::json::array();
        for (const auto& item : args["files"]) {
            if (!item.is_string()) return error_result("invalid_arguments", "files entries must be strings");
            std::string file = item.get<std::string>();
            std::error_code ec;
            if (!std::filesystem::exists(path_from_utf8(file), ec)) {
                return error_result("file_not_found", "upload file does not exist: " + file);
            }
            files.push_back(file);
        }
        nlohmann::json bridge_args;
        bridge_args["selector"] = target;
        bridge_args["files"] = std::move(files);
        return command_result(state, ctx, args, "upload", std::move(bridge_args));
    };
    return impl;
}

ToolImpl make_enable_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_enable";
    def.description =
        "Enable additional browser tool groups for subsequent model requests: interaction, pointer, capture, network, diagnostics, or advanced.";
    def.parameters = object_schema({
        {"groups", {{"type", "array"},
                    {"items", {{"type", "string"},
                               {"enum", {"interaction", "pointer", "capture", "network",
                                         "diagnostics", "advanced"}}}},
                    {"description", "Tool groups to enable."}}},
    }, {"groups"});

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = false;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) return error_result("invalid_arguments", error);
        if (!args.contains("groups") || !args["groups"].is_array() || args["groups"].empty()) {
            return error_result("invalid_arguments", "browser_enable requires a non-empty groups array");
        }

        std::vector<std::string> requested;
        for (const auto& g : args["groups"]) {
            if (!g.is_string()) return error_result("invalid_arguments", "groups entries must be strings");
            std::string group = g.get<std::string>();
            if (group_tools().find(group) == group_tools().end()) {
                return error_result("invalid_arguments", "unknown browser tool group: " + group);
            }
            requested.push_back(group);
        }

        if (state->config.tool_mode == "compact") {
            nlohmann::json out;
            out["ok"] = true;
            out["data"] = {
                {"tool_mode", state->config.tool_mode},
                {"enabled_tool_groups", enabled_groups_snapshot(state)},
                {"message", "compact mode keeps only the core browser tools registered"},
            };
            return ToolResult{out.dump(2), true};
        }

        for (const auto& group : requested) {
            if (!is_group_enabled(state, group)) {
                set_group_enabled(state, group);
                if (ctx.tool_executor) {
                    register_tool_group(*ctx.tool_executor, state, group);
                }
            }
        }

        nlohmann::json out;
        out["ok"] = true;
        out["data"] = {
            {"tool_mode", state->config.tool_mode},
            {"enabled_tool_groups", enabled_groups_snapshot(state)},
            {"enabled_now", requested},
            {"guidance", "Use browser_read_page first and prefer @e refs returned by the latest snapshot."},
        };
        return ToolResult{out.dump(2), true};
    };
    return impl;
}

ToolImpl make_tool_by_name(const std::shared_ptr<BrowserToolState>& state,
                           const std::string& name) {
    if (name == "browser_status") return make_status_tool(state);
    if (name == "browser_open") return make_open_tool(state);
    if (name == "browser_find_tab") return make_find_tab_tool(state);
    if (name == "browser_navigate") return make_navigate_tool(state);
    if (name == "browser_read_page") return make_read_page_tool(state);
    if (name == "browser_wait") return make_wait_tool(state);
    if (name == "browser_enable") return make_enable_tool(state);
    if (name == "browser_close_session") return make_close_session_tool(state);
    if (name == "browser_click") return make_click_tool(state);
    if (name == "browser_fill") return make_fill_tool(state);
    if (name == "browser_type") return make_type_tool(state);
    if (name == "browser_hover") return make_hover_tool(state);
    if (name == "browser_drag") return make_drag_tool(state);
    if (name == "browser_scroll") return make_scroll_tool(state);
    if (name == "browser_network") return make_network_tool(state);
    if (name == "browser_screenshot") return make_screenshot_tool(state);
    if (name == "browser_save_pdf") return make_save_pdf_tool(state);
    if (name == "browser_trace") return make_trace_tool(state);
    if (name == "browser_list_tabs") return make_list_tabs_tool(state);
    if (name == "browser_evaluate") return make_evaluate_tool(state);
    if (name == "browser_upload") return make_upload_tool(state);
    return make_status_tool(state);
}

void register_tool_group(ToolExecutor& tools,
                         const std::shared_ptr<BrowserToolState>& state,
                         const std::string& group) {
    auto it = group_tools().find(group);
    if (it == group_tools().end()) return;
    for (const auto& name : it->second) {
        tools.register_tool(make_tool_by_name(state, name));
    }
}

} // namespace

std::vector<std::string> ace_browser_core_tool_names() {
    return {
        "browser_status",
        "browser_open",
        "browser_find_tab",
        "browser_navigate",
        "browser_read_page",
        "browser_wait",
        "browser_enable",
        "browser_close_session",
    };
}

std::vector<std::string> ace_browser_full_tool_names() {
    std::vector<std::string> names = ace_browser_core_tool_names();
    for (const auto& [_, tools] : group_tools()) {
        names.insert(names.end(), tools.begin(), tools.end());
    }
    return names;
}

std::vector<std::string> ace_browser_group_names() {
    std::vector<std::string> names;
    for (const auto& [group, _] : group_tools()) names.push_back(group);
    return names;
}

void register_ace_browser_bridge_tools(ToolExecutor& tools,
                                       const AceBrowserBridgeConfig& config,
                                       CliRunner runner) {
    if (!config.enabled) return;

    auto state = std::make_shared<BrowserToolState>();
    state->config = config;
    state->client = std::make_shared<AceBrowserBridgeClient>(config, std::move(runner));
    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->enabled_groups.insert("core");
    }

    for (const auto& name : ace_browser_core_tool_names()) {
        tools.register_tool(make_tool_by_name(state, name));
    }

    if (config.tool_mode == "full") {
        for (const auto& [group, _] : group_tools()) {
            set_group_enabled(state, group);
            register_tool_group(tools, state, group);
        }
    }
}

std::size_t unregister_ace_browser_bridge_tools(ToolExecutor& tools) {
    std::size_t removed = 0;
    for (const auto& name : ace_browser_full_tool_names()) {
        if (tools.unregister_tool(name)) ++removed;
    }
    return removed;
}

} // namespace acecode::ace_browser_bridge
