#include "builtin_commands.hpp"
#include "compact.hpp"
#include "../provider/model_context_resolver.hpp"
#include "../tool/mcp_manager.hpp"
#include "../tool/tool_executor.hpp"
#include "../skills/skill_registry.hpp"
#include "../skills/skill_commands.hpp"
#include <mutex>
#include <sstream>
#include <iomanip>
#include <thread>

namespace acecode {

static void cmd_help(CommandContext& ctx, const std::string& /*args*/) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Available commands:\n"
        << "  /help     - Show this help message\n"
        << "  /clear    - Clear conversation history\n"
        << "  /compact  - Compress conversation history\n"
        << "  /model    - Show or switch current model\n"
        << "  /config   - Show current configuration\n"
        << "  /cost     - Show token usage and estimated cost\n"
        << "  /resume   - Resume a previous session\n"
        << "  /mcp      - Manage MCP servers\n"
        << "  /skills   - List, invoke, or reload installed skills\n"
        << "  /exit     - Exit acecode";

    if (ctx.skills) {
        size_t n = ctx.skills->list().size();
        if (n > 0) {
            oss << "\n\n" << n << " skill" << (n == 1 ? "" : "s")
                << " installed. Type /skills for the full list, or /skills help for usage.";
        }
    }
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_clear(CommandContext& ctx, const std::string& /*args*/) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.state.conversation.clear();
    ctx.agent_loop.clear_messages();
    ctx.token_tracker.reset();
    ctx.state.token_status.clear();
    if (ctx.session_manager) {
        ctx.session_manager->end_current_session();
    }
    ctx.state.conversation.push_back({"system", "Conversation cleared.", false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_model(CommandContext& ctx, const std::string& args) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    if (args.empty()) {
        std::string info = "[" + ctx.provider.name() + "] model: " + ctx.provider.model();
        ctx.state.conversation.push_back({"system", info, false});
    } else {
        ctx.provider.set_model(args);
        ctx.config.context_window = resolve_model_context_window(
            ctx.config,
            ctx.provider.name(),
            ctx.provider.model(),
            ctx.config.context_window
        );
        ctx.agent_loop.set_context_window(ctx.config.context_window);
        ctx.state.token_status = ctx.token_tracker.format_status(ctx.config.context_window);
        ctx.state.status_line = "[" + ctx.provider.name() + "] model: " + args;
        ctx.state.conversation.push_back({"system", "Model switched to: " + args, false});
    }
    ctx.state.chat_follow_tail = true;
}

static void cmd_config(CommandContext& ctx, const std::string& /*args*/) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Current configuration:\n"
        << "  provider:       " << ctx.config.provider << "\n"
        << "  model:          " << ctx.provider.model() << "\n"
        << "  context_window: " << ctx.config.context_window << "\n"
        << "  permission:     " << PermissionManager::mode_name(ctx.permissions.mode());
    if (ctx.config.provider == "openai") {
        oss << "\n  base_url:       " << ctx.config.openai.base_url;
    }
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_cost(CommandContext& ctx, const std::string& /*args*/) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Session token usage:\n"
        << "  prompt:     " << TokenTracker::format_tokens(ctx.token_tracker.prompt_tokens()) << "\n"
        << "  completion: " << TokenTracker::format_tokens(ctx.token_tracker.completion_tokens()) << "\n"
        << "  total:      " << TokenTracker::format_tokens(ctx.token_tracker.total_tokens()) << "\n";
    oss << std::fixed;
    oss.precision(4);
    oss << "  est. cost:  ~$" << ctx.token_tracker.estimated_cost();
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_compact(CommandContext& ctx, const std::string& /*args*/) {
    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);

        // Reject if already compacting
        if (ctx.state.is_compacting) {
            ctx.state.conversation.push_back({"system", "Compaction already in progress.", false});
            ctx.state.chat_follow_tail = true;
            return;
        }

        ctx.state.is_compacting = true;
        ctx.state.compact_abort_requested.store(false);
        ctx.state.conversation.push_back({"system", "Compacting conversation...", false});
        ctx.state.chat_follow_tail = true;
    }

    // Join any previous compact thread before launching a new one
    if (ctx.state.compact_thread.joinable()) {
        ctx.state.compact_thread.join();
    }

    // Capture references for the background thread
    auto& provider   = ctx.provider;
    auto& agent_loop = ctx.agent_loop;
    auto& state      = ctx.state;
    auto post_event  = ctx.post_event;

    ctx.state.compact_thread = std::thread([&provider, &agent_loop, &state, post_event]() {
        auto result = compact_context(provider, agent_loop, state, 4, false,
                                      &state.compact_abort_requested);

        {
            std::lock_guard<std::mutex> lk(state.mu);
            if (!result.performed) {
                state.conversation.push_back({"system", result.error, false});
            } else {
                std::ostringstream oss;
                oss << "Compacted " << result.messages_compressed << " messages, saved ~"
                    << TokenTracker::format_tokens(result.estimated_tokens_saved) << " tokens";
                state.conversation.push_back({"system", oss.str(), false});
            }
            state.is_compacting = false;
            state.chat_follow_tail = true;
        }

        if (post_event) post_event();
    });
}

static const char* mcp_state_label(McpServerState s) {
    switch (s) {
        case McpServerState::Connected: return "connected";
        case McpServerState::Disabled:  return "disabled";
        case McpServerState::Failed:    return "failed";
    }
    return "unknown";
}

static void mcp_push(CommandContext& ctx, const std::string& msg) {
    ctx.state.conversation.push_back({"system", msg, false});
    ctx.state.chat_follow_tail = true;
}

static std::string mcp_known_servers(const McpManager& mgr) {
    auto names = mgr.server_names();
    if (names.empty()) return "(none)";
    std::ostringstream oss;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) oss << ", ";
        oss << names[i];
    }
    return oss.str();
}

static void cmd_mcp(CommandContext& ctx, const std::string& args) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);

    if (!ctx.mcp_manager || !ctx.tools) {
        mcp_push(ctx, "MCP manager is not available in this session.");
        return;
    }
    McpManager& mgr = *ctx.mcp_manager;
    ToolExecutor& tools = *ctx.tools;

    // Parse: first token is subcommand, remainder is name.
    std::string trimmed = args;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }

    std::string sub, name;
    if (!trimmed.empty()) {
        auto sp = trimmed.find(' ');
        if (sp == std::string::npos) {
            sub = trimmed;
        } else {
            sub = trimmed.substr(0, sp);
            name = trimmed.substr(sp + 1);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
                name.erase(name.begin());
            }
        }
    }

    // Default view: list servers with state summary.
    if (sub.empty()) {
        auto servers = mgr.list_servers();
        if (servers.empty()) {
            mcp_push(ctx, "No MCP servers configured.");
            return;
        }
        std::ostringstream oss;
        oss << "MCP servers:";
        for (const auto& s : servers) {
            oss << "\n  " << s.name
                << "  [" << mcp_state_label(s.state) << "]"
                << "  [" << s.transport << "]"
                << "  tools=" << s.tool_count
                << "  at=" << s.command_line;
        }
        mcp_push(ctx, oss.str());
        return;
    }

    if (sub == "help") {
        std::ostringstream oss;
        oss << "/mcp usage:\n"
            << "  /mcp                    - List servers and status\n"
            << "  /mcp list               - List tools grouped by server\n"
            << "  /mcp enable <name>      - Connect a disabled or failed server\n"
            << "  /mcp disable <name>     - Stop a server and unregister its tools\n"
            << "  /mcp reconnect <name>   - Force a teardown + reconnect\n"
            << "  /mcp help               - Show this help";
        mcp_push(ctx, oss.str());
        return;
    }

    if (sub == "list") {
        auto grouped = mgr.list_tools_by_server();
        if (grouped.empty()) {
            mcp_push(ctx, "No MCP servers configured.");
            return;
        }
        auto servers = mgr.list_servers();
        std::map<std::string, McpServerState> state_map;
        for (const auto& s : servers) state_map[s.name] = s.state;

        std::ostringstream oss;
        oss << "MCP tools:";
        for (const auto& [server, defs] : grouped) {
            auto it = state_map.find(server);
            const char* label = (it != state_map.end()) ? mcp_state_label(it->second) : "unknown";
            oss << "\n  " << server << "  [" << label << "]";
            if (defs.empty()) {
                oss << "\n    (no tools registered)";
            } else {
                for (const auto& d : defs) {
                    oss << "\n    - " << d.name;
                    if (!d.description.empty()) {
                        std::string desc = d.description;
                        if (desc.size() > 80) desc = desc.substr(0, 77) + "...";
                        oss << "  " << desc;
                    }
                }
            }
        }
        mcp_push(ctx, oss.str());
        return;
    }

    if (sub == "disable" || sub == "enable" || sub == "reconnect") {
        if (name.empty()) {
            mcp_push(ctx, "Usage: /mcp " + sub + " <server-name>");
            return;
        }
        if (!mgr.has_server(name)) {
            mcp_push(ctx, "Unknown MCP server '" + name + "'. Known: " + mcp_known_servers(mgr));
            return;
        }

        bool changed = false;
        if (sub == "disable") {
            changed = mgr.disable(name, tools);
            if (changed) {
                mcp_push(ctx, "Disabled MCP server '" + name + "'.");
            } else {
                mcp_push(ctx, "MCP server '" + name + "' is already disabled.");
            }
        } else if (sub == "enable") {
            changed = mgr.enable(name, tools);
            if (changed) {
                mcp_push(ctx, "Enabled MCP server '" + name + "'.");
            } else {
                // Distinguish already-connected vs failed.
                auto servers = mgr.list_servers();
                for (const auto& s : servers) {
                    if (s.name == name) {
                        if (s.state == McpServerState::Connected) {
                            mcp_push(ctx, "MCP server '" + name + "' is already connected.");
                        } else {
                            mcp_push(ctx, "Failed to enable MCP server '" + name + "'. Check logs for details.");
                        }
                        return;
                    }
                }
            }
        } else { // reconnect
            changed = mgr.reconnect(name, tools);
            if (changed) {
                mcp_push(ctx, "Reconnected MCP server '" + name + "'.");
            } else {
                mcp_push(ctx, "Failed to reconnect MCP server '" + name + "'. Check logs for details.");
            }
        }
        return;
    }

    mcp_push(ctx, "Unknown /mcp subcommand '" + sub + "'. Try /mcp help.");
}

static void cmd_exit(CommandContext& ctx, const std::string& /*args*/) {
    if (ctx.request_exit) {
        ctx.request_exit();
    }
}

static void cmd_skills(CommandContext& ctx, const std::string& args) {
    if (!ctx.skills) {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system",
            "Skill system is not available in this session.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    // Trim args.
    std::string trimmed = args;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }

    std::string sub = trimmed;
    {
        auto sp = trimmed.find(' ');
        if (sp != std::string::npos) sub = trimmed.substr(0, sp);
    }

    if (sub == "help") {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        std::ostringstream oss;
        oss << "/skills usage:\n"
            << "  /skills              - List installed skills\n"
            << "  /skills list         - Same as /skills\n"
            << "  /skills reload       - Rescan skill directories and refresh commands\n"
            << "  /skills help         - Show this help\n"
            << "\n"
            << "To invoke a skill, type /<skill-name> [optional args].";
        ctx.state.conversation.push_back({"system", oss.str(), false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    if (sub == "reload") {
        size_t before = ctx.skills->list().size();
        size_t after = before;
        if (ctx.command_registry) {
            after = reload_skill_commands(*ctx.command_registry, *ctx.skills);
        } else {
            ctx.skills->reload();
            after = ctx.skills->list().size();
        }
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        std::ostringstream oss;
        oss << "Reloaded skills: " << after << " registered (was " << before << ").";
        if (!ctx.command_registry) {
            oss << "\n(Command registry not available — /<skill> bindings are only refreshed on restart.)";
        }
        ctx.state.conversation.push_back({"system", oss.str(), false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    // Default + "list": render full skill table.
    auto skills = ctx.skills->list();
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    if (skills.empty()) {
        oss << "No skills installed. Add a SKILL.md under ~/.acecode/skills/<category>/<name>/ and rerun `/skills reload` (then restart to pick up new /<name> commands).";
    } else {
        oss << "Installed skills (" << skills.size() << "):";
        std::string last_cat;
        for (const auto& s : skills) {
            if (s.category != last_cat) {
                oss << "\n";
                if (!s.category.empty()) {
                    oss << "  [" << s.category << "]\n";
                } else {
                    oss << "  [uncategorized]\n";
                }
                last_cat = s.category;
            }
            oss << "    /" << s.command_key;
            if (s.command_key != s.name) oss << "  (name: " << s.name << ")";
            if (!s.description.empty()) {
                std::string desc = s.description;
                if (desc.size() > 120) desc = desc.substr(0, 117) + "...";
                oss << "\n        " << desc;
            }
            oss << "\n";
        }
    }
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void do_resume_session(CommandContext& ctx, const std::string& session_id,
                              const std::vector<SessionMeta>& sessions) {
    // Caller must hold ctx.state.mu
    // Find the meta for this session
    const SessionMeta* target = nullptr;
    for (const auto& s : sessions) {
        if (s.id == session_id) {
            target = &s;
            break;
        }
    }

    auto messages = ctx.session_manager->resume_session(session_id);
    ctx.agent_loop.clear_messages();
    for (const auto& msg : messages) {
        ctx.agent_loop.push_message(msg);
    }
    ctx.state.conversation.clear();
    for (const auto& msg : messages) {
        bool is_tool = (msg.role == "tool");
        ctx.state.conversation.push_back({msg.role, msg.content, is_tool});
    }
    std::ostringstream oss;
    oss << "Resumed session " << session_id << " (" << messages.size() << " messages)";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_resume(CommandContext& ctx, const std::string& args) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    if (!ctx.session_manager) {
        ctx.state.conversation.push_back({"system", "Session persistence is not available.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    auto sessions = ctx.session_manager->list_sessions();
    if (sessions.empty()) {
        ctx.state.conversation.push_back({"system", "No previous sessions found for this project.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    // If a number argument is provided, resume that session directly
    if (!args.empty()) {
        int choice = 0;
        try {
            choice = std::stoi(args);
        } catch (...) {
            ctx.state.conversation.push_back({"system", "Invalid session number: " + args, false});
            ctx.state.chat_follow_tail = true;
            return;
        }
        if (choice < 1 || choice > static_cast<int>(sessions.size())) {
            ctx.state.conversation.push_back({"system",
                "Session number out of range. Use /resume to see available sessions.", false});
            ctx.state.chat_follow_tail = true;
            return;
        }
        do_resume_session(ctx, sessions[choice - 1].id, sessions);
        return;
    }

    // Build picker items
    int max_show = std::min(static_cast<int>(sessions.size()), 20);
    ctx.state.resume_items.clear();
    for (int i = 0; i < max_show; ++i) {
        const auto& s = sessions[i];
        std::ostringstream line;
        line << "[" << (i + 1) << "] " << s.updated_at
             << "  " << s.message_count << " msgs";
        if (!s.summary.empty()) {
            line << "  " << s.summary;
        }
        ctx.state.resume_items.push_back({s.id, line.str()});
    }
    ctx.state.resume_selected = 0;
    ctx.state.resume_picker_active = true;

    // Capture session list for callback
    auto captured_sessions = sessions;
    auto* sm = ctx.session_manager;
    auto* al = &ctx.agent_loop;
    ctx.state.resume_callback = [&state = ctx.state, sm, al, captured_sessions](const std::string& sid) {
        auto messages = sm->resume_session(sid);
        al->clear_messages();
        for (const auto& msg : messages) {
            al->push_message(msg);
        }
        state.conversation.clear();
        for (const auto& msg : messages) {
            bool is_tool = (msg.role == "tool");
            state.conversation.push_back({msg.role, msg.content, is_tool});
        }
        std::ostringstream oss;
        oss << "Resumed session " << sid << " (" << messages.size() << " messages)";
        state.conversation.push_back({"system", oss.str(), false});
        state.chat_follow_tail = true;
    };
}

void register_builtin_commands(CommandRegistry& registry) {
    registry.register_command({"help", "Show available commands", cmd_help});
    registry.register_command({"clear", "Clear conversation history", cmd_clear});
    registry.register_command({"model", "Show or switch current model", cmd_model});
    registry.register_command({"config", "Show current configuration", cmd_config});
    registry.register_command({"cost", "Show token usage and estimated cost", cmd_cost});
    registry.register_command({"compact", "Compress conversation history", cmd_compact});
    registry.register_command({"resume", "Resume a previous session", cmd_resume});
    registry.register_command({"mcp", "Manage MCP servers", cmd_mcp});
    registry.register_command({"skills", "List, invoke, or reload installed skills", cmd_skills});
    registry.register_command({"exit", "Exit acecode", cmd_exit});
}

} // namespace acecode
