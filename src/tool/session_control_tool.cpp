#include "session_control_tool.hpp"

#include "../session/session_control_service.hpp"
#include "../session/session_manager.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace acecode {

namespace {

using nlohmann::json;

ToolResult error_result(const std::string& message) {
    return ToolResult{"[Error] " + message, false};
}

ToolResult service_result(SessionControlResult result) {
    if (!result.success) return error_result(result.error);
    ToolResult output;
    output.output = result.value.dump();
    return output;
}

std::optional<json> parse_object(const std::string& arguments,
                                 ToolResult* error) {
    try {
        auto value = json::parse(arguments);
        if (!value.is_object()) {
            *error = error_result("arguments must be an object");
            return std::nullopt;
        }
        return value;
    } catch (const std::exception& ex) {
        *error = error_result(std::string("invalid arguments: ") + ex.what());
        return std::nullopt;
    }
}

SessionControlScope scope_from_context(const ToolContext& ctx) {
    SessionControlScope scope;
    scope.cwd = ctx.cwd;
    if (!ctx.session_manager) return scope;
    scope.caller_session_id = ctx.session_manager->current_session_id();
    if (!scope.caller_session_id.empty()) {
        const auto meta = ctx.session_manager->load_session_meta(
            scope.caller_session_id);
        if (!meta.cwd.empty()) scope.cwd = meta.cwd;
    }
    return scope;
}

std::string string_arg(const json& args, const char* name) {
    return args.contains(name) && args[name].is_string()
        ? args[name].get<std::string>()
        : std::string{};
}

bool bool_arg(const json& args, const char* name, bool fallback = false) {
    return args.contains(name) && args[name].is_boolean()
        ? args[name].get<bool>() : fallback;
}

int int_arg(const json& args, const char* name, int fallback = 0) {
    return args.contains(name) && args[name].is_number_integer()
        ? args[name].get<int>() : fallback;
}

} // namespace

ToolImpl create_session_query_tool(
    std::shared_ptr<SessionControlToolDeps> deps) {
    ToolImpl tool;
    tool.definition.name = "session_query";
    tool.definition.description =
        "Inspect ACECode sessions in the current workspace with bounded output. "
        "Actions: list returns one page of summaries; read returns metadata and "
        "the latest bounded user/assistant result; wait returns only events after "
        "a cursor; open_controls makes the low-frequency session_control schema "
        "available to this session's next model request. This tool never returns "
        "a full transcript.";
    tool.definition.parameters = json{
        {"type", "object"},
        {"properties", json{
            {"action", json{{"type", "string"},
                {"enum", json::array({"list", "read", "wait", "open_controls"})}}},
            {"session_id", json{{"type", "string"}}},
            {"cursor", json{{"oneOf", json::array({
                json{{"type", "string"}}, json{{"type", "integer"}}
            })}}},
            {"page_size", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}},
            {"max_bytes", json{{"type", "integer"}, {"minimum", 256}, {"maximum", 8192}}},
            {"timeout_seconds", json{{"type", "integer"}, {"minimum", 0}, {"maximum", 60}}},
            {"archived", json{{"type", "boolean"}}},
            {"capability", json{{"type", "string"}, {"enum", json::array({"session_control"})}}},
        }}},
        {"required", json::array({"action"})},
    };
    tool.is_read_only = true;
    tool.execute = [deps](const std::string& arguments,
                          const ToolContext& ctx) -> ToolResult {
        ToolResult parse_error;
        auto parsed = parse_object(arguments, &parse_error);
        if (!parsed) return parse_error;
        const auto& args = *parsed;
        const std::string action = string_arg(args, "action");

        if (action == "open_controls") {
            const std::string capability = string_arg(args, "capability");
            if (!capability.empty() && capability != "session_control") {
                return error_result("unknown or unavailable capability");
            }
            if (!ctx.enable_deferred_tool ||
                !ctx.enable_deferred_tool("session_control")) {
                return error_result(
                    "session_control is unavailable under the current capability policy");
            }
            return ToolResult{
                R"({"enabled":"session_control","effective":"next_model_request"})",
                true};
        }
        if (!deps || !deps->service) {
            return error_result("session queries are unavailable in this runtime");
        }

        const auto scope = scope_from_context(ctx);
        if (action == "list") {
            return service_result(deps->service->list(
                scope,
                static_cast<std::size_t>((std::max)(1, int_arg(args, "page_size", 10))),
                string_arg(args, "cursor"),
                bool_arg(args, "archived")));
        }
        if (action == "read") {
            return service_result(deps->service->read(
                scope, string_arg(args, "session_id"),
                static_cast<std::size_t>((std::max)(256, int_arg(args, "max_bytes", 4096)))));
        }
        if (action == "wait") {
            std::uint64_t cursor = 0;
            if (args.contains("cursor") && args["cursor"].is_number_unsigned()) {
                cursor = args["cursor"].get<std::uint64_t>();
            } else if (args.contains("cursor") && args["cursor"].is_number_integer()) {
                const auto raw = args["cursor"].get<std::int64_t>();
                if (raw > 0) cursor = static_cast<std::uint64_t>(raw);
            }
            return service_result(deps->service->wait(
                scope, string_arg(args, "session_id"), cursor,
                int_arg(args, "timeout_seconds", 30), ctx.abort_flag));
        }
        return error_result("unknown query action");
    };
    return tool;
}

ToolImpl create_session_control_tool(
    std::shared_ptr<SessionControlToolDeps> deps) {
    ToolImpl tool;
    tool.definition.name = "session_control";
    tool.definition.description =
        "Mutate ACECode sessions inside the current workspace after explicit "
        "capability opening and normal tool permission approval. Actions: create, "
        "fork, send, interrupt, title, archive, pin. Permanent deletion, purge, "
        "bulk cleanup, cross-workspace access, and subagent-tree control are not "
        "supported.";
    tool.definition.parameters = json{
        {"type", "object"},
        {"properties", json{
            {"action", json{{"type", "string"}, {"enum", json::array({
                "create", "fork", "send", "interrupt", "title", "archive", "pin"
            })}}},
            {"session_id", json{{"type", "string"}}},
            {"title", json{{"type", "string"}, {"maxLength", 160}}},
            {"model", json{{"type", "string"}}},
            {"message", json{{"type", "string"}}},
            {"at_message_id", json{{"type", "string"}}},
            {"steer_if_busy", json{{"type", "boolean"}}},
            {"archived", json{{"type", "boolean"}}},
            {"pinned", json{{"type", "boolean"}}},
        }}},
        {"required", json::array({"action"})},
    };
    tool.defer_loading = true;
    tool.is_read_only = false;
    tool.execute = [deps](const std::string& arguments,
                          const ToolContext& ctx) -> ToolResult {
        if (!deps || !deps->service) {
            return error_result("session control is unavailable in this runtime");
        }
        ToolResult parse_error;
        auto parsed = parse_object(arguments, &parse_error);
        if (!parsed) return parse_error;
        const auto& args = *parsed;
        const auto scope = scope_from_context(ctx);
        const std::string action = string_arg(args, "action");
        const std::string id = string_arg(args, "session_id");

        if (action == "create") {
            return service_result(deps->service->create(
                scope, string_arg(args, "title"), string_arg(args, "model")));
        }
        if (action == "fork") {
            return service_result(deps->service->fork(
                scope, id, string_arg(args, "title"),
                string_arg(args, "at_message_id")));
        }
        if (action == "send") {
            return service_result(deps->service->send(
                scope, id, string_arg(args, "message"),
                bool_arg(args, "steer_if_busy", true)));
        }
        if (action == "interrupt") {
            return service_result(deps->service->interrupt(scope, id));
        }
        if (action == "title") {
            return service_result(deps->service->set_title(
                scope, id, string_arg(args, "title")));
        }
        if (action == "archive") {
            return service_result(deps->service->set_archived(
                scope, id, bool_arg(args, "archived", true)));
        }
        if (action == "pin") {
            return service_result(deps->service->set_pinned(
                scope, id, bool_arg(args, "pinned", true)));
        }
        if (action == "delete" || action == "purge" ||
            action == "cleanup") {
            return error_result("permanent deletion and purge are not supported");
        }
        return error_result("unknown control action; permanent deletion is not supported");
    };
    return tool;
}

} // namespace acecode
