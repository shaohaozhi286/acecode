#include "plan_mode_tool.hpp"

#include "../session/session_manager.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

namespace acecode {

namespace {

ToolResult tool_error(const std::string& message) {
    return ToolResult{"[Error] " + message, false};
}

ToolDef enter_plan_mode_def() {
    ToolDef def;
    def.name = "EnterPlanMode";
    def.description =
        "Request permission to enter plan mode for non-trivial implementation "
        "tasks that require exploration and design before coding. In plan mode, "
        "explore the codebase, write the plan file, and do not edit normal "
        "workspace files until the plan is approved.";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()},
        {"additionalProperties", false},
    };
    return def;
}

ToolDef exit_plan_mode_def() {
    ToolDef def;
    def.name = "ExitPlanMode";
    def.description =
        "Use this when you are in plan mode, have finished writing the plan "
        "to the session plan file, and are ready for user approval. This tool "
        "does not take the plan content as a parameter; it reads the plan file.";
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"allowedPrompts", {
                {"type", "array"},
                {"description", "Optional semantic permissions needed to implement the plan."},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"tool", {{"type", "string"}}},
                        {"prompt", {{"type", "string"}}},
                    }},
                }},
            }},
        }},
        {"additionalProperties", true},
    };
    return def;
}

std::string plan_mode_enter_result(const std::string& plan_file) {
    std::ostringstream oss;
    oss << "Entered plan mode. Focus on exploring the codebase and designing "
        << "an implementation approach.\n\n";
    if (!plan_file.empty()) {
        oss << "Plan file: " << plan_file << "\n\n";
    }
    oss << "DO NOT write or edit any files except the plan file. "
        << "When the plan is ready for approval, call ExitPlanMode.";
    return oss.str();
}

std::string plan_mode_yolo_noop_result() {
    return "OK";
}

} // namespace

ToolImpl create_enter_plan_mode_tool() {
    ToolImpl impl;
    impl.definition = enter_plan_mode_def();
    impl.is_read_only = false;
    impl.execute = [](const std::string&, const ToolContext& ctx) -> ToolResult {
        if (!ctx.enter_plan_mode) {
            return tool_error("plan mode is not available in this context");
        }
        const std::string mode = ctx.current_permission_mode
            ? ctx.current_permission_mode()
            : std::string{};
        if (mode == "yolo") {
            return ToolResult{plan_mode_yolo_noop_result(), true};
        }
        std::string plan_file = ctx.enter_plan_mode();
        return ToolResult{plan_mode_enter_result(plan_file), true};
    };
    return impl;
}

ToolImpl create_exit_plan_mode_tool() {
    ToolImpl impl;
    impl.definition = exit_plan_mode_def();
    impl.is_read_only = false;
    impl.execute = [](const std::string&, const ToolContext& ctx) -> ToolResult {
        const std::string mode = ctx.current_permission_mode
            ? ctx.current_permission_mode()
            : std::string{};
        if (mode == "yolo") {
            return ToolResult{plan_mode_yolo_noop_result(), true};
        }
        if (mode != "plan") {
            return tool_error(
                "You are not in plan mode. This tool is only for exiting plan mode after writing a plan.");
        }
        if (!ctx.session_manager) {
            return tool_error("active session is not available");
        }

        const std::string file_path = ctx.session_manager->ensure_plan_file_path();
        std::string plan = ctx.session_manager->read_plan_file();
        const std::string restored = ctx.exit_plan_mode
            ? ctx.exit_plan_mode()
            : std::string{"default"};

        if (plan.empty()) {
            return ToolResult{
                "User has approved exiting plan mode. You can now proceed. "
                "Restored permission mode: " + restored,
                true};
        }

        std::ostringstream oss;
        oss << "User has approved your plan. You can now start coding. "
            << "Start with updating your todo list if applicable.\n\n"
            << "Your plan has been saved to: " << file_path << "\n"
            << "Restored permission mode: " << restored << "\n\n"
            << "## Approved Plan:\n"
            << plan;
        return ToolResult{oss.str(), true};
    };
    return impl;
}

} // namespace acecode
