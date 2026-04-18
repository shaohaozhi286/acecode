#include "skill_commands.hpp"

#include "skill_activation.hpp"
#include "skill_registry.hpp"
#include "../agent_loop.hpp"
#include "../commands/command_registry.hpp"
#include "../tui_state.hpp"
#include "../utils/logger.hpp"

#include <mutex>

namespace acecode {

namespace {

std::string truncate_description(const std::string& desc, size_t max_len = 80) {
    if (desc.size() <= max_len) return desc;
    if (max_len <= 3) return desc.substr(0, max_len);
    return desc.substr(0, max_len - 3) + "...";
}

} // namespace

std::vector<std::string> register_skill_commands(CommandRegistry& cmd_registry,
                                                 SkillRegistry& skill_registry) {
    std::vector<std::string> registered;
    auto skills = skill_registry.list();
    for (const auto& meta : skills) {
        const std::string& key = meta.command_key;
        if (key.empty()) continue;
        if (cmd_registry.has_command(key)) {
            LOG_WARN("[skills] command /" + key +
                     " collides with existing built-in; skill '" + meta.name + "' not registered as slash command");
            continue;
        }

        SlashCommand cmd;
        cmd.name = key;
        cmd.description = truncate_description(meta.description);
        std::string skill_name = meta.name;
        cmd.execute = [&skill_registry, skill_name](CommandContext& ctx, const std::string& args) {
            auto found = skill_registry.find(skill_name);
            if (!found) {
                std::lock_guard<std::mutex> lk(ctx.state.mu);
                ctx.state.conversation.push_back({"system",
                    "Skill '" + skill_name + "' is no longer available (registry may have been reloaded).",
                    false});
                ctx.state.chat_follow_tail = true;
                return;
            }

            std::string body = skill_registry.read_skill_body(skill_name);
            auto files = skill_registry.list_supporting_files(skill_name);
            std::string message = build_activation_message(*found, body, files, args);

            {
                std::lock_guard<std::mutex> lk(ctx.state.mu);
                ctx.state.conversation.push_back({"system",
                    "[Invoking skill: " + skill_name + "]", false});
                ctx.state.chat_follow_tail = true;
                if (ctx.state.is_waiting) {
                    ctx.state.pending_queue.push_back(message);
                    return;
                }
                ctx.state.is_waiting = true;
            }

            ctx.agent_loop.submit(message);
        };

        cmd_registry.register_command(cmd);
        registered.push_back(key);
    }
    return registered;
}

void unregister_skill_commands(CommandRegistry& cmd_registry,
                               const std::vector<std::string>& command_keys) {
    for (const auto& k : command_keys) {
        cmd_registry.unregister_command(k);
    }
}

namespace {
std::mutex g_tracked_mu;
std::vector<std::string> g_tracked_keys;
}

std::vector<std::string> register_skill_commands_tracked(CommandRegistry& cmd_registry,
                                                         SkillRegistry& skill_registry) {
    auto keys = register_skill_commands(cmd_registry, skill_registry);
    {
        std::lock_guard<std::mutex> lk(g_tracked_mu);
        g_tracked_keys = keys;
    }
    return keys;
}

size_t reload_skill_commands(CommandRegistry& cmd_registry,
                             SkillRegistry& skill_registry) {
    std::vector<std::string> old_keys;
    {
        std::lock_guard<std::mutex> lk(g_tracked_mu);
        old_keys = g_tracked_keys;
        g_tracked_keys.clear();
    }
    unregister_skill_commands(cmd_registry, old_keys);
    skill_registry.reload();
    auto new_keys = register_skill_commands(cmd_registry, skill_registry);
    {
        std::lock_guard<std::mutex> lk(g_tracked_mu);
        g_tracked_keys = new_keys;
    }
    return new_keys.size();
}

} // namespace acecode
