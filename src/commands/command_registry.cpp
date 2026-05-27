#include "command_registry.hpp"
#include "../skills/skill_activation.hpp"
#include "../skills/skill_registry.hpp"

#include <chrono>
#include <mutex>
#include <sstream>

namespace acecode {

void CommandRegistry::register_command(const SlashCommand& cmd) {
    commands_[cmd.name] = cmd;
}

bool CommandRegistry::unregister_command(const std::string& name) {
    auto it = commands_.find(name);
    if (it == commands_.end()) return false;
    commands_.erase(it);
    return true;
}

bool CommandRegistry::dispatch(const std::string& input, CommandContext& ctx) {
    if (input.empty() || input[0] != '/') return false;

    // Parse: "/command args..."
    std::string trimmed = input.substr(1); // remove leading '/'
    std::string cmd_name;
    std::string args;

    size_t space_pos = trimmed.find(' ');
    if (space_pos == std::string::npos) {
        cmd_name = trimmed;
    } else {
        cmd_name = trimmed.substr(0, space_pos);
        args = trimmed.substr(space_pos + 1);
    }

    auto it = commands_.find(cmd_name);
    if (it == commands_.end()) {
        if (ctx.skills) {
            auto skill = ctx.skills->find(cmd_name);
            if (skill) {
                std::string message = build_skill_invocation_hint(*skill, args);
                {
                    std::lock_guard<std::mutex> lk(ctx.state.mu);
                    ctx.state.conversation.push_back(
                        {"system", "[Invoking skill: " + skill->name + "]", false});
                    ctx.state.chat_follow_tail = true;
                    if (ctx.state.is_waiting) {
                        ctx.state.pending_queue.push_back(message);
                        return true;
                    }
                    ctx.state.thinking_start_time = std::chrono::steady_clock::now();
                    ctx.state.streaming_output_chars = 0;
                    ctx.state.last_completion_tokens_authoritative = 0;
                    ctx.state.is_waiting = true;
                }
                ctx.agent_loop.submit(message);
                return true;
            }
        }

        ctx.state.conversation.push_back(
            {"system", "Unknown command: /" + cmd_name + ". Type /help for available commands.", false});
        ctx.state.chat_follow_tail = true;
        return true; // consumed the input (even though command unknown)
    }

    it->second.execute(ctx, args);
    return true;
}

} // namespace acecode
