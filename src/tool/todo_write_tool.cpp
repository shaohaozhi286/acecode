#include "todo_write_tool.hpp"

#include "../session/session_manager.hpp"
#include "../session/todo_state.hpp"
#include "tool_icons.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace acecode {

namespace {

ToolResult tool_error(const std::string& message) {
    return ToolResult{"[Error] " + message, false};
}

constexpr const char* kTodoWriteDescription =
    "Manage the visible task checklist for the current session. Use this for "
    "complex tasks with 3 or more steps, whenever the user provides multiple "
    "tasks, or when you need to show progress across a multi-step request.\n\n"
    "Call TodoWrite before starting substantial work to publish the plan. "
    "Keep exactly one item in_progress at a time. Mark an item completed "
    "immediately after it is truly done, then move the next item to "
    "in_progress. Do not wait until the end to update the checklist.\n\n"
    "Reading: omit todos to read the current list.\n"
    "Writing: provide todos. With merge=false or omitted, replace the entire "
    "list. With merge=true, update existing items by id and append new ids. "
    "Each item is {id, content, status}, where status is pending, "
    "in_progress, completed, or cancelled. List order is priority. Always "
    "returns the full current list and summary.";

ToolDef todo_write_def() {
    ToolDef def;
    def.name = "TodoWrite";
    def.description = kTodoWriteDescription;
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"todos", {
                {"type", "array"},
                {"description", "Task items to write. Omit this field to read the current list."},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"id", {
                            {"type", "string"},
                            {"description", "Unique stable identifier for this task item."},
                        }},
                        {"content", {
                            {"type", "string"},
                            {"description", "Task description shown to the user."},
                        }},
                        {"status", {
                            {"type", "string"},
                            {"enum", {"pending", "in_progress", "completed", "cancelled"}},
                            {"description", "Current task status."},
                        }},
                    }},
                    {"required", {"id", "content", "status"}},
                    {"additionalProperties", false},
                }},
            }},
            {"merge", {
                {"type", "boolean"},
                {"description", "true updates existing items by id and appends new items; false replaces the list."},
                {"default", false},
            }},
        }},
        {"additionalProperties", false},
    };
    return def;
}

std::string summary_object(const nlohmann::json& summary, bool wrote) {
    const int total = summary.value("total", 0);
    const int in_progress = summary.value("in_progress", 0);
    const int pending = summary.value("pending", 0);
    const int completed = summary.value("completed", 0);
    std::ostringstream oss;
    oss << (wrote ? "tasks updated" : "tasks read") << " (" << total << ")";
    if (in_progress > 0) oss << ", active " << in_progress;
    if (pending > 0) oss << ", pending " << pending;
    if (completed > 0) oss << ", completed " << completed;
    return oss.str();
}

ToolResult execute_todo_write(const std::string& arguments_json, const ToolContext& ctx) {
    if (!ctx.session_manager) {
        return tool_error("active session is not available");
    }

    nlohmann::json args;
    try {
        args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json);
    } catch (const std::exception& e) {
        return tool_error(std::string("invalid JSON: ") + e.what());
    }
    if (!args.is_object()) {
        return tool_error("TodoWrite arguments must be an object");
    }

    const bool has_todos = args.contains("todos") && !args["todos"].is_null();
    const bool merge = args.value("merge", false);
    if (has_todos && !args["todos"].is_array()) {
        return tool_error("TodoWrite todos must be an array when provided");
    }

    const std::string session_id = ctx.session_manager->ensure_active_session_id();
    if (session_id.empty()) {
        return tool_error("session is not available");
    }

    std::vector<TodoItem> items = ctx.session_manager->current_todos();
    if (has_todos) {
        items = merge
            ? merge_todo_items_from_json(items, args["todos"])
            : replace_todo_items_from_json(args["todos"]);
        ctx.session_manager->set_todos(items);
    }

    nlohmann::json payload = todo_payload_to_json(session_id, items);
    if (ctx.emit_todo_updated) {
        ctx.emit_todo_updated(payload);
    }

    ToolSummary sum;
    sum.verb = has_todos ? (merge ? "updated" : "planned") : "read";
    sum.object = summary_object(payload["summary"], has_todos);
    sum.icon = tool_icon("TodoWrite");
    sum.metrics.emplace_back("total", std::to_string(payload["summary"].value("total", 0)));
    sum.metrics.emplace_back("done", std::to_string(payload["summary"].value("completed", 0)));

    ToolResult result;
    result.output = payload.dump(2);
    result.success = true;
    result.summary = std::move(sum);
    return result;
}

} // namespace

ToolImpl create_todo_write_tool() {
    ToolImpl impl;
    impl.definition = todo_write_def();
    impl.execute = execute_todo_write;
    impl.is_read_only = false;
    impl.source = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
