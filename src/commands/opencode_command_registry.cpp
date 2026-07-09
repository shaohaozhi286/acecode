#include "opencode_command_registry.hpp"

#include "command_registry.hpp"
#include "opencode_command.hpp"
#include "../agent_loop.hpp"
#include "../tui_state.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <mutex>

namespace acecode {

namespace {

std::string truncate_description(const std::string& desc, size_t max_len = 80) {
    if (desc.size() <= max_len) return desc;
    if (max_len <= 3) return desc.substr(0, max_len);
    size_t cut = max_len - 3;
    while (cut > 0 && (static_cast<unsigned char>(desc[cut]) & 0xC0) == 0x80) --cut;
    return desc.substr(0, cut) + "...";
}

void submit_opencode_command(CommandContext& ctx,
                             const OpencodeCommandInfo& command,
                             const std::string& args) {
    const std::string prompt = expand_opencode_command(command, args);
    std::string display = "/" + command.name;
    if (!args.empty()) display += " " + args;

    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back(
            {"system", "[Invoking command: /" + command.name + "]", false});
        ctx.state.chat_follow_tail = true;
        if (ctx.state.is_waiting) {
            ctx.state.pending_queue.push_back(prompt);
            return;
        }
        ctx.state.thinking_start_time = std::chrono::steady_clock::now();
        ctx.state.streaming_output_chars = 0;
        ctx.state.last_completion_tokens_authoritative = 0;
        ctx.state.is_waiting = true;
    }

    submit_user_text(ctx, prompt, display);
}

std::mutex g_tracked_mu;
std::vector<std::string> g_tracked_keys;

} // namespace

std::vector<std::string> register_opencode_commands(
    CommandRegistry& cmd_registry,
    const AppConfig& config,
    const std::string& working_dir) {
    std::vector<std::string> registered;
    for (const auto& command : load_opencode_commands(config, working_dir)) {
        if (command.name.empty()) continue;
        if (cmd_registry.has_command(command.name)) {
            LOG_WARN("[commands] opencode command /" + command.name +
                     " collides with existing slash command; skipping " +
                     command.source_path.string());
            continue;
        }

        SlashCommand slash;
        slash.name = command.name;
        slash.description = truncate_description(command.description);
        OpencodeCommandInfo captured = command;
        slash.execute = [captured](CommandContext& ctx, const std::string& args) {
            submit_opencode_command(ctx, captured, args);
        };
        cmd_registry.register_command(slash);
        registered.push_back(command.name);
    }
    return registered;
}

void unregister_opencode_commands(
    CommandRegistry& cmd_registry,
    const std::vector<std::string>& command_keys) {
    for (const auto& key : command_keys) {
        cmd_registry.unregister_command(key);
    }
}

std::vector<std::string> register_opencode_commands_tracked(
    CommandRegistry& cmd_registry,
    const AppConfig& config,
    const std::string& working_dir) {
    auto keys = register_opencode_commands(cmd_registry, config, working_dir);
    {
        std::lock_guard<std::mutex> lk(g_tracked_mu);
        g_tracked_keys = keys;
    }
    return keys;
}

std::size_t reload_opencode_commands(
    CommandRegistry& cmd_registry,
    const AppConfig& config,
    const std::string& working_dir) {
    std::vector<std::string> old_keys;
    {
        std::lock_guard<std::mutex> lk(g_tracked_mu);
        old_keys = g_tracked_keys;
        g_tracked_keys.clear();
    }
    unregister_opencode_commands(cmd_registry, old_keys);
    auto new_keys = register_opencode_commands(cmd_registry, config, working_dir);
    {
        std::lock_guard<std::mutex> lk(g_tracked_mu);
        g_tracked_keys = new_keys;
    }
    return new_keys.size();
}

} // namespace acecode
