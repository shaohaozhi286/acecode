#include "goal_tool.hpp"

#include "../session/session_manager.hpp"
#include "../session/thread_goal_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>

namespace acecode {

namespace {

ToolResult tool_error(const std::string& message) {
    return ToolResult{"[Error] " + message, false};
}

ThreadGoalStore* require_goal_store(const ToolContext& ctx, std::string* session_id) {
    if (!ctx.session_manager) return nullptr;
    if (session_id) *session_id = ctx.session_manager->current_session_id();
    return ctx.session_manager->goal_store();
}

std::string goal_payload_dump(const std::optional<ThreadGoal>& goal) {
    nlohmann::json out;
    if (goal.has_value()) {
        out["goal"] = thread_goal_to_json(*goal);
    } else {
        out["goal"] = nullptr;
    }
    return out.dump(2);
}

std::optional<std::int64_t> parse_optional_budget(const nlohmann::json& args,
                                                  std::string* error) {
    if (!args.contains("token_budget") || args["token_budget"].is_null()) {
        return std::nullopt;
    }
    if (!args["token_budget"].is_number_integer()) {
        if (error) *error = "token_budget must be a positive integer";
        return std::nullopt;
    }
    std::int64_t value = args["token_budget"].get<std::int64_t>();
    if (value <= 0) {
        if (error) *error = "token_budget must be a positive integer";
        return std::nullopt;
    }
    return value;
}

ToolDef get_goal_def() {
    ToolDef def;
    def.name = "get_goal";
    def.description =
        "Get the current thread goal, including status, budget, tokens used, "
        "elapsed time, and remaining token budget when present.";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()},
        {"additionalProperties", false},
    };
    return def;
}

ToolDef create_goal_def() {
    ToolDef def;
    def.name = "create_goal";
    def.description =
        "Create a new thread goal only when the user explicitly asks for a goal "
        "and no goal already exists. Do not infer goals from ordinary tasks.";
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"objective", {
                {"type", "string"},
                {"description", "Concrete objective to pursue."},
            }},
            {"token_budget", {
                {"type", {"integer", "null"}},
                {"description", "Optional positive token budget."},
            }},
        }},
        {"required", {"objective"}},
        {"additionalProperties", false},
    };
    return def;
}

ToolDef update_goal_def() {
    ToolDef def;
    def.name = "update_goal";
    def.description =
        "Update the current goal. Use this tool only to mark the goal achieved "
        "or genuinely blocked. Set status \"complete\" only when the objective "
        "is actually achieved and no required work remains. Set status "
        "\"blocked\" only when the same blocking condition has repeated for at "
        "least three consecutive goal turns and the agent cannot make meaningful "
        "progress without user input or an external-state change. Do not use "
        "\"blocked\" merely because the work is hard, slow, uncertain, "
        "incomplete, or would benefit from clarification.";
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"status", {
                {"type", "string"},
                {"enum", {"complete", "blocked"}},
                {"description", "Allowed values are complete and blocked."},
            }},
        }},
        {"required", {"status"}},
        {"additionalProperties", false},
    };
    return def;
}

} // namespace

ToolImpl create_get_goal_tool() {
    ToolImpl impl;
    impl.definition = get_goal_def();
    impl.is_read_only = true;
    impl.execute = [](const std::string&, const ToolContext& ctx) -> ToolResult {
        std::string session_id;
        ThreadGoalStore* store = require_goal_store(ctx, &session_id);
        if (!store) return tool_error("goal storage is not available");
        if (session_id.empty()) return ToolResult{goal_payload_dump(std::nullopt), true};

        std::string error;
        auto goal = store->get_thread_goal(session_id, &error);
        if (!error.empty()) return tool_error(error);
        return ToolResult{goal_payload_dump(goal), true};
    };
    return impl;
}

ToolImpl create_create_goal_tool() {
    ToolImpl impl;
    impl.definition = create_goal_def();
    impl.is_read_only = true;
    impl.execute = [](const std::string& arguments_json, const ToolContext& ctx) -> ToolResult {
        if (!ctx.session_manager) return tool_error("goal storage is not available");
        const std::string session_id = ctx.session_manager->ensure_active_session_id();
        if (session_id.empty()) return tool_error("session is not available");
        ThreadGoalStore* store = ctx.session_manager->goal_store();
        if (!store) return tool_error("goal storage is not available");

        nlohmann::json args;
        try {
            args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json);
        } catch (const std::exception& e) {
            return tool_error(std::string("invalid JSON: ") + e.what());
        }
        if (!args.contains("objective") || !args["objective"].is_string()) {
            return tool_error("create_goal requires objective");
        }

        std::string error;
        auto existing = store->get_thread_goal(session_id, &error);
        if (!error.empty()) return tool_error(error);
        if (existing.has_value()) {
            return tool_error("A goal already exists. Do not replace an existing goal from a model tool; ask the user to use /goal.");
        }

        auto budget = parse_optional_budget(args, &error);
        if (!error.empty()) return tool_error(error);
        const std::string objective = trim_goal_objective(args["objective"].get<std::string>());
        if (!store->replace_thread_goal(session_id, objective, budget, ThreadGoalStatus::Active, &error)) {
            return tool_error(error.empty() ? "failed to create goal" : error);
        }

        auto goal = store->get_thread_goal(session_id, &error);
        if (goal.has_value() && ctx.emit_goal_updated) {
            ctx.emit_goal_updated(thread_goal_to_json(*goal));
        }
        return ToolResult{goal_payload_dump(goal), true};
    };
    return impl;
}

ToolImpl create_update_goal_tool() {
    ToolImpl impl;
    impl.definition = update_goal_def();
    impl.is_read_only = true;
    impl.execute = [](const std::string& arguments_json, const ToolContext& ctx) -> ToolResult {
        if (!ctx.session_manager) return tool_error("goal storage is not available");
        const std::string session_id = ctx.session_manager->current_session_id();
        if (session_id.empty()) return tool_error("no current session");
        ThreadGoalStore* store = ctx.session_manager->goal_store();
        if (!store) return tool_error("goal storage is not available");

        nlohmann::json args;
        try {
            args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json);
        } catch (const std::exception& e) {
            return tool_error(std::string("invalid JSON: ") + e.what());
        }
        const std::string status = args.value("status", "");
        std::optional<ThreadGoalStatus> target_status;
        if (status == "complete") {
            target_status = ThreadGoalStatus::Complete;
        } else if (status == "blocked") {
            target_status = ThreadGoalStatus::Blocked;
        } else {
            return tool_error("update_goal only supports status \"complete\" or \"blocked\"");
        }

        if (ctx.account_goal_usage) ctx.account_goal_usage();

        std::string error;
        auto goal = store->get_thread_goal(session_id, &error);
        if (!error.empty()) return tool_error(error);
        if (!goal.has_value()) return tool_error("no goal exists");

        if (!store->update_thread_goal_status(session_id, goal->goal_id, *target_status, &error)) {
            return tool_error(error.empty() ? "failed to update goal" : error);
        }
        auto updated = store->get_thread_goal(session_id, &error);
        if (updated.has_value() && ctx.emit_goal_updated) {
            ctx.emit_goal_updated(thread_goal_to_json(*updated));
        }

        nlohmann::json out;
        out["goal"] = updated.has_value() ? thread_goal_to_json(*updated) : nlohmann::json(nullptr);
        out["final"] = *target_status == ThreadGoalStatus::Complete;
        if (updated.has_value() && updated->token_budget.has_value()) {
            out["token_budget"] = *updated->token_budget;
            out["tokens_used"] = updated->tokens_used;
            out["remaining_tokens"] = std::max<std::int64_t>(0, *updated->token_budget - updated->tokens_used);
        }
        return ToolResult{out.dump(2), true};
    };
    return impl;
}

} // namespace acecode
