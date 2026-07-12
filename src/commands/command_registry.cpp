#include "command_registry.hpp"
#include "opencode_command_registry.hpp"
#include "../skills/skill_activation.hpp"
#include "../skills/skill_commands.hpp"
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
        if (!ctx.cwd.empty()) {
            reload_opencode_commands(*this, ctx.config, ctx.cwd);
            it = commands_.find(cmd_name);
            if (it != commands_.end()) {
                it->second.execute(ctx, args);
                return true;
            }
        }
        // 未命中:先就着磁盘重扫一次并重绑 skill 斜杠命令(会话进行中新写到磁盘的
        // skill 才能第一次敲就用上,且回填 commands_ 让后续自动补全/`/help` 列得出),
        // 然后再查一次。reload 走已测过的 reload_skill_commands(只动 skill key,
        // 内建命令不受影响)。command_registry 即 *this。
        if (ctx.skills) {
            reload_skill_commands(*this, *ctx.skills);
            it = commands_.find(cmd_name);
            if (it != commands_.end()) {
                it->second.execute(ctx, args);
                return true;
            }
        }
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
                    ctx.state.turn_completion_tokens_confirmed = 0;
                    ctx.state.is_waiting = true;
                }
                submit_user_text(ctx, message);
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
