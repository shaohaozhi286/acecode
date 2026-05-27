#include "goal_command.hpp"

#include "../session/thread_goal_store.hpp"
#include "../utils/token_tracker.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace acecode {

namespace {

std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

void push_goal_message(CommandContext& ctx, const std::string& message) {
    ctx.state.conversation.push_back({"system", message, false});
    ctx.state.chat_follow_tail = true;
}

std::string format_elapsed(std::int64_t seconds) {
    if (seconds < 60) return std::to_string(std::max<std::int64_t>(0, seconds)) + "s";
    const auto minutes = seconds / 60;
    const auto rest = seconds % 60;
    if (minutes < 60) {
        return std::to_string(minutes) + "m " + std::to_string(rest) + "s";
    }
    const auto hours = minutes / 60;
    return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
}

std::string format_goal_summary(const ThreadGoal& goal) {
    std::ostringstream oss;
    oss << "Goal:\n"
        << "  objective: " << goal.objective << "\n"
        << "  status:    " << to_string(goal.status) << "\n"
        << "  tokens:    " << TokenTracker::format_tokens(goal.tokens_used);
    if (goal.token_budget.has_value()) {
        oss << " / " << TokenTracker::format_tokens(*goal.token_budget)
            << " (" << TokenTracker::format_tokens(
                    std::max<std::int64_t>(0, *goal.token_budget - goal.tokens_used))
            << " remaining)";
    }
    oss << "\n"
        << "  elapsed:   " << format_elapsed(goal.time_used_seconds) << "\n\n"
        << "Commands: /goal edit <objective>, /goal pause, /goal resume, /goal clear";
    return oss.str();
}

std::string format_goal_status_chip(const ThreadGoal& goal) {
    std::ostringstream oss;
    oss << "goal: " << to_string(goal.status) << " "
        << TokenTracker::format_tokens(static_cast<int>(std::min<std::int64_t>(
            goal.tokens_used, static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    if (goal.token_budget.has_value()) {
        oss << "/" << TokenTracker::format_tokens(static_cast<int>(std::min<std::int64_t>(
            *goal.token_budget, static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    }
    return oss.str();
}

bool parse_budget_value(const std::string& text, std::int64_t* out) {
    if (!out || text.empty()) return false;
    std::string s = lower_ascii(text);
    std::int64_t multiplier = 1;
    char suffix = s.back();
    if (suffix == 'k' || suffix == 'm') {
        multiplier = suffix == 'k' ? 1000 : 1000000;
        s.pop_back();
    }
    if (s.empty()) return false;
    std::int64_t value = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        value = value * 10 + (c - '0');
        if (value <= 0) return false;
    }
    *out = value * multiplier;
    return *out > 0;
}

struct ParsedGoalArgs {
    std::optional<std::int64_t> token_budget;
    std::string remainder;
    std::string error;
};

ParsedGoalArgs parse_goal_args(std::string args) {
    ParsedGoalArgs parsed;
    args = trim_ascii(std::move(args));
    if (args.rfind("--tokens", 0) != 0) {
        parsed.remainder = args;
        return parsed;
    }

    std::string rest = trim_ascii(args.substr(std::string("--tokens").size()));
    if (rest.empty()) {
        parsed.error = "Usage: /goal --tokens <positive-int>[K|M] <objective>";
        return parsed;
    }
    const auto split = rest.find_first_of(" \t\r\n");
    const std::string budget_text = split == std::string::npos ? rest : rest.substr(0, split);
    std::int64_t budget = 0;
    if (!parse_budget_value(budget_text, &budget)) {
        parsed.error = "Goal token budget must be a positive integer, optionally suffixed with K or M.";
        return parsed;
    }
    parsed.token_budget = budget;
    parsed.remainder = split == std::string::npos ? std::string{} : trim_ascii(rest.substr(split + 1));
    return parsed;
}

void emit_goal_updated(CommandContext& ctx, const ThreadGoal& goal) {
    ctx.agent_loop.events().emit(SessionEventKind::GoalUpdated,
        nlohmann::json{{"session_id", goal.thread_id}, {"goal", thread_goal_to_json(goal)}});
    ctx.state.goal_status = format_goal_status_chip(goal);
    ctx.agent_loop.restore_goal_runtime();
}

void emit_goal_cleared(CommandContext& ctx, const std::string& session_id) {
    ctx.agent_loop.events().emit(SessionEventKind::GoalCleared,
        nlohmann::json{{"session_id", session_id}});
    ctx.state.goal_status.clear();
    ctx.agent_loop.restore_goal_runtime();
}

void cmd_goal(CommandContext& ctx, const std::string& raw_args) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    if (!ctx.session_manager) {
        push_goal_message(ctx, "Goal storage is not available.");
        return;
    }
    ThreadGoalStore* store = ctx.session_manager->goal_store();
    if (!store) {
        push_goal_message(ctx, "Goal storage is not available.");
        return;
    }

    std::string args = trim_ascii(raw_args);
    const std::string lower = lower_ascii(args);

    if (args.empty() || lower == "view") {
        const std::string sid = ctx.session_manager->current_session_id();
        if (sid.empty()) {
            push_goal_message(ctx, "No goal set. Use /goal <objective> to create one.");
            return;
        }
        std::string error;
        auto goal = store->get_thread_goal(sid, &error);
        if (!error.empty()) {
            push_goal_message(ctx, "Goal error: " + error);
            return;
        }
        if (!goal.has_value()) {
            push_goal_message(ctx, "No goal set. Use /goal <objective> to create one.");
            return;
        }
        push_goal_message(ctx, format_goal_summary(*goal));
        return;
    }

    const auto first_space = args.find_first_of(" \t\r\n");
    const std::string sub = lower_ascii(first_space == std::string::npos
        ? args
        : args.substr(0, first_space));
    const std::string tail = first_space == std::string::npos
        ? std::string{}
        : trim_ascii(args.substr(first_space + 1));

    const std::string sid = (sub == "clear" || sub == "pause" || sub == "resume" || sub == "edit")
        ? ctx.session_manager->current_session_id()
        : ctx.session_manager->ensure_active_session_id();
    if (sid.empty()) {
        push_goal_message(ctx, "No active session is available for /goal.");
        return;
    }

    std::string error;
    auto current = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        push_goal_message(ctx, "Goal error: " + error);
        return;
    }

    if (sub == "clear") {
        if (!current.has_value()) {
            push_goal_message(ctx, "No goal to clear.");
            return;
        }
        if (!store->delete_thread_goal(sid, &error)) {
            push_goal_message(ctx, "Goal error: " + error);
            return;
        }
        emit_goal_cleared(ctx, sid);
        push_goal_message(ctx, "Goal cleared.");
        return;
    }

    if (sub == "pause") {
        if (!current.has_value()) {
            push_goal_message(ctx, "No goal to pause.");
            return;
        }
        if (current->status != ThreadGoalStatus::Active) {
            push_goal_message(ctx, "Goal is not active.");
            return;
        }
        if (!store->update_thread_goal_status(sid, current->goal_id, ThreadGoalStatus::Paused, &error)) {
            push_goal_message(ctx, "Goal error: " + error);
            return;
        }
        auto goal = store->get_thread_goal(sid);
        if (goal.has_value()) emit_goal_updated(ctx, *goal);
        push_goal_message(ctx, "Goal paused.");
        return;
    }

    if (sub == "resume") {
        if (!current.has_value()) {
            push_goal_message(ctx, "No goal to resume.");
            return;
        }
        if (current->status == ThreadGoalStatus::Complete) {
            push_goal_message(ctx, "Goal is already complete.");
            return;
        }
        if (current->token_budget.has_value() &&
            current->tokens_used >= *current->token_budget) {
            push_goal_message(ctx, "Goal is over its token budget. Create a replacement goal with a larger budget.");
            return;
        }
        if (!store->update_thread_goal_status(sid, current->goal_id, ThreadGoalStatus::Active, &error)) {
            push_goal_message(ctx, "Goal error: " + error);
            return;
        }
        auto goal = store->get_thread_goal(sid);
        if (goal.has_value()) emit_goal_updated(ctx, *goal);
        push_goal_message(ctx, "Goal resumed.");
        ctx.agent_loop.clear_stale_abort_request();
        ctx.agent_loop.maybe_continue_goal();
        return;
    }

    if (sub == "edit") {
        if (!current.has_value()) {
            push_goal_message(ctx, "No goal to edit.");
            return;
        }
        ParsedGoalArgs parsed = parse_goal_args(tail);
        if (!parsed.error.empty()) {
            push_goal_message(ctx, parsed.error);
            return;
        }
        const std::string objective = trim_goal_objective(parsed.remainder);
        if (!validate_goal_objective(objective, &error)) {
            push_goal_message(ctx, error);
            return;
        }
        auto budget = parsed.token_budget.has_value() ? parsed.token_budget : current->token_budget;
        if (!store->update_thread_goal_objective(sid, current->goal_id, objective, budget, &error)) {
            push_goal_message(ctx, "Goal error: " + error);
            return;
        }
        auto goal = store->get_thread_goal(sid);
        if (goal.has_value()) emit_goal_updated(ctx, *goal);
        push_goal_message(ctx, goal.has_value() ? format_goal_summary(*goal) : "Goal updated.");
        return;
    }

    ParsedGoalArgs parsed = parse_goal_args(args);
    if (!parsed.error.empty()) {
        push_goal_message(ctx, parsed.error);
        return;
    }
    const std::string objective = trim_goal_objective(parsed.remainder);
    if (!validate_goal_objective(objective, &error)) {
        push_goal_message(ctx, error);
        return;
    }
    if (!store->replace_thread_goal(sid, objective, parsed.token_budget, ThreadGoalStatus::Active, &error)) {
        push_goal_message(ctx, "Goal error: " + error);
        return;
    }
    auto goal = store->get_thread_goal(sid);
    if (goal.has_value()) emit_goal_updated(ctx, *goal);
    push_goal_message(ctx, goal.has_value() ? format_goal_summary(*goal) : "Goal created.");
    ctx.agent_loop.maybe_continue_goal();
}

} // namespace

void register_goal_command(CommandRegistry& registry) {
    registry.register_command({"goal", "Create, view, pause, resume, edit, or clear the thread goal", cmd_goal});
}

} // namespace acecode
