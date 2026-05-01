#include "builtin_commands.hpp"
#include "compact.hpp"
#include "history_command.hpp"
#include "init_command.hpp"
#include "memory_command.hpp"
#include "models_command.hpp"
#include "proxy_command.hpp"
#include "websearch_command.hpp"
#include "../config/config.hpp"
#include "../config/saved_models.hpp"
#include "../provider/model_context_resolver.hpp"
#include "../provider/model_resolver.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/provider_swap.hpp"
#include "../tool/mcp_manager.hpp"
#include "../tool/tool_executor.hpp"
#include "../skills/skill_registry.hpp"
#include "../skills/skill_commands.hpp"
#include "../session/session_manager.hpp"
#include "../session/session_resume_restore.hpp"
#include "../session/session_rewind.hpp"
#include "../utils/terminal_title.hpp"
#include <algorithm>
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
        << "  /tokens   - Show session token usage\n"
        << "  /resume   - Resume a previous session\n"
        << "  /rewind   - Rewind to a previous user turn\n"
        << "  /mcp      - Manage MCP servers\n"
        << "  /skills   - List, invoke, or reload installed skills\n"
        << "  /memory   - List, view, edit, forget, or reload persistent user memory\n"
        << "  /init     - Generate an ACECODE.md skeleton in the current directory\n"
        << "  /history  - List or clear the per-working-directory input history\n"
        << "  /models   - Inspect bundled models.dev registry\n"
        << "  /proxy    - Show or switch the HTTP proxy used for LLM/API requests\n"
        << "  /title    - Set or show the window title for this session\n"
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
    if (!ctx.state.current_session_title.empty()) {
        ctx.state.current_session_title.clear();
        clear_terminal_title();
    }
    ctx.state.conversation.push_back({"system", "Conversation cleared.", false});
    ctx.state.chat_follow_tail = true;
}

// 在 saved_models 中按 name 找;失败时如果 name == "(legacy)" 返回 legacy
// entry。其它情况返回 nullopt。调用方负责报"未知 name"错误。
static std::optional<ModelProfile> lookup_entry_by_name(const AppConfig& cfg,
                                                      const std::string& name) {
    if (name == "(legacy)") return synth_legacy_entry(cfg);
    for (const auto& e : cfg.saved_models) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

// 把切换结果反馈到 TUI。调用前已做完 swap;这里只更新状态行 / 系统消息。
// 注意:调用方 MUST NOT 持 ctx.state.mu —— 本函数自己加锁。
static void announce_switch(CommandContext& ctx, const ModelProfile& entry,
                            const std::string& scope_note) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.agent_loop.set_context_window(ctx.config.context_window);
    ctx.state.token_status = ctx.token_tracker.format_status(ctx.config.context_window);
    std::ostringstream oss;
    oss << "Switched to " << entry.name
        << " (" << entry.provider << "/" << entry.model << ")";
    if (!scope_note.empty()) oss << " " << scope_note;
    std::string info = oss.str();
    ctx.state.status_line = info;
    ctx.state.conversation.push_back({"system", info, false});
    ctx.state.chat_follow_tail = true;
}

// 报"未知 name"错误。同 announce 一样自己加锁。
static void report_unknown_name(CommandContext& ctx, const std::string& name) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Unknown model name: " << name
        << ". Run /model to pick from available.";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

// 当前 effective entry —— 用 name 与 saved_models 匹配;命中 saved_models 则
// 复用,否则合成 legacy entry。仅用于 picker 高亮当前选中行。
static std::string current_effective_name(CommandContext& ctx) {
    std::string pname = ctx.provider.name();
    std::string pmodel = ctx.provider.model();
    for (const auto& e : ctx.config.saved_models) {
        if (e.provider == pname && e.model == pmodel) return e.name;
    }
    return "(legacy)";
}

// 简单 trim helper —— args 里可能有多余空格。
static std::string trim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

// 解析 args 为 (flag, name) 对。flag ∈ {"", "--cwd", "--default"}。
// 返回 false 表示参数格式无效。
static bool parse_model_args(const std::string& raw, std::string& flag,
                             std::string& name) {
    flag.clear();
    name.clear();
    std::string s = trim(raw);
    if (s.empty()) return true;  // 无参 → picker

    if (s.rfind("--", 0) == 0) {
        // --cwd <name> 或 --default <name>
        auto sp = s.find_first_of(" \t");
        if (sp == std::string::npos) {
            // 只给了 flag 没给 name
            flag = s;
            return false;
        }
        flag = s.substr(0, sp);
        name = trim(s.substr(sp + 1));
        if (flag != "--cwd" && flag != "--default") return false;
        if (name.empty()) return false;
        return true;
    }

    // 直接是 name —— in-memory 切换
    name = s;
    return true;
}

// 渲染 picker 列表(纯文本回退,无 FTXUI)。覆盖 saved_models + 兜底 (legacy)。
// 当前 effective 行加 "*"。这是最小骨架 —— 真正的 FTXUI picker(任务 5.2)
// 留待后续(picker 与 TuiState/screen 耦合,本 phase 用文本展示先解锁验证)。
static void render_model_picker(CommandContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Available models:\n";
    std::string current = current_effective_name(ctx);
    bool legacy_listed_in_saved = false;
    for (const auto& e : ctx.config.saved_models) {
        if (e.name == "(legacy)") legacy_listed_in_saved = true;
        oss << "  " << (e.name == current ? "*" : " ")
            << " " << e.name << "  (" << e.provider << "/" << e.model << ")\n";
    }
    if (!legacy_listed_in_saved) {
        ModelProfile legacy = synth_legacy_entry(ctx.config);
        oss << "  " << (legacy.name == current ? "*" : " ")
            << " " << legacy.name << "  (" << legacy.provider << "/"
            << legacy.model << ")\n";
    }
    oss << "\nUse: /model <name>           - switch this session (in-memory)\n"
        << "     /model --cwd <name>     - switch + persist to this directory\n"
        << "     /model --default <name> - switch + persist as global default";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

static void cmd_model(CommandContext& ctx, const std::string& args) {
    std::string flag, name;
    if (!parse_model_args(args, flag, name)) {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system",
            "Usage: /model | /model <name> | /model --cwd <name> | /model --default <name>",
            false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    // 无参 → 列表(picker 占位)
    if (flag.empty() && name.empty()) {
        render_model_picker(ctx);
        return;
    }

    auto entry = lookup_entry_by_name(ctx.config, name);
    if (!entry.has_value()) {
        report_unknown_name(ctx, name);
        return;
    }

    // 切换前保护:provider_handle / provider_mu 缺失则退化为旧行为。理论
    // 上启动期 main.cpp 总会传入这俩,但 init_command 的测试桩不会。
    if (ctx.provider_handle && ctx.provider_mu) {
        swap_provider_if_needed(*ctx.provider_handle, *ctx.provider_mu, *entry, ctx.config);
    } else {
        ctx.provider.set_model(entry->model);
        ctx.config.context_window = resolve_model_context_window(
            ctx.config, ctx.provider.name(), ctx.provider.model(),
            ctx.config.context_window);
    }

    std::string scope_note;
    if (flag == "--cwd") {
        save_cwd_model_override(ctx.cwd, name);
        scope_note = "[persisted to cwd]";
    } else if (flag == "--default") {
        ctx.config.default_model_name = name;
        save_config(ctx.config);
        scope_note = "[persisted as default]";
    }
    announce_switch(ctx, *entry, scope_note);
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

static void cmd_tokens(CommandContext& ctx, const std::string& /*args*/) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Session token usage:\n"
        << "  prompt:     " << TokenTracker::format_tokens(ctx.token_tracker.prompt_tokens()) << "\n"
        << "  completion: " << TokenTracker::format_tokens(ctx.token_tracker.completion_tokens()) << "\n"
        << "  total:      " << TokenTracker::format_tokens(ctx.token_tracker.total_tokens());
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

static void cmd_title(CommandContext& ctx, const std::string& args) {
    // Trim leading/trailing whitespace.
    std::string text = args;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }

    std::lock_guard<std::mutex> lk(ctx.state.mu);

    // No args: echo current title.
    if (text.empty()) {
        if (ctx.state.current_session_title.empty()) {
            ctx.state.conversation.push_back({"system",
                "No title set. Use /title <text> to set one.", false});
        } else {
            ctx.state.conversation.push_back({"system",
                "Current title: " + ctx.state.current_session_title, false});
        }
        ctx.state.chat_follow_tail = true;
        return;
    }

    // Clear: literal `clear` or quoted empty string.
    if (text == "clear" || text == "\"\"") {
        ctx.state.current_session_title.clear();
        clear_terminal_title();
        if (ctx.session_manager) {
            ctx.session_manager->set_session_title("");
        }
        ctx.state.status_line = "Title cleared";
        ctx.state.conversation.push_back({"system", "Title cleared.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    // Strip surrounding double quotes if user wrapped the value.
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }

    std::string err;
    if (!sanitize_title(text, err)) {
        ctx.state.conversation.push_back({"system",
            "Title contains invalid control characters.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    set_terminal_title(text);
    ctx.state.current_session_title = text;

    std::string status = "Title set: " + text;
    if (err == "truncated") status += " (truncated to 256 bytes)";
    ctx.state.status_line = status;

    if (ctx.session_manager) {
        ctx.session_manager->set_session_title(text);
        ctx.state.conversation.push_back({"system", status, false});
    } else {
        ctx.state.conversation.push_back({"system", status + " (not persisted)", false});
    }
    ctx.state.chat_follow_tail = true;
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
            << "Built-in skill roots include project/home .acecode/skills and compatible .agent/skills directories.\n"
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
        oss << "No skills installed. Add a SKILL.md under ~/.acecode/skills/<category>/<name>/ or ~/.agent/skills/<category>/<name>/ and rerun `/skills reload` (then restart to pick up new /<name> commands).";
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

    // 与 --resume CLI 路径(main.cpp)对齐:resume 前先把 meta 的 provider+model
    // 应用到运行时,可能触发 swap_provider_if_needed。design D6 / 任务 6.3。
    if (target && ctx.provider_handle && ctx.provider_mu &&
        !target->provider.empty() && !target->model.empty()) {
        auto cwd_override = load_cwd_model_override(ctx.cwd);
        ModelProfile resumed_entry = resolve_effective_model(
            ctx.config, cwd_override, std::optional<SessionMeta>{*target});
        swap_provider_if_needed(*ctx.provider_handle, *ctx.provider_mu, resumed_entry, ctx.config);
        ctx.session_manager->set_active_provider(
            ctx.provider_handle->get()->name(), ctx.provider_handle->get()->model());
        if (resumed_entry.name.rfind("(session:", 0) == 0) {
            ctx.state.conversation.push_back({"system",
                "⚠ Resumed with ad-hoc model entry (session recorded " +
                target->provider + "/" + target->model +
                ", not in saved_models). Use /model --default <name> to pick a permanent one.",
                false});
        }
    }

    auto messages = ctx.session_manager->resume_session(session_id);
    ctx.agent_loop.clear_messages();
    ctx.state.conversation.clear();
    ToolExecutor fallback_tools;
    const ToolExecutor& replay_tools = ctx.tools ? *ctx.tools : fallback_tools;
    append_resumed_session_messages(messages, ctx.state, ctx.agent_loop, replay_tools);
    std::ostringstream oss;
    oss << "Resumed session " << session_id << " (" << messages.size() << " messages)";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;

    if (target && !target->title.empty()) {
        set_terminal_title(target->title);
        ctx.state.current_session_title = target->title;
    } else {
        ctx.state.current_session_title.clear();
    }
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

    // Build picker items. SessionManager::list_sessions() already truncates
    // to config.max_sessions, so render every entry it returned — viewport
    // scrolling in the TUI handles the case where the list does not fit.
    ctx.state.resume_items.clear();
    for (size_t i = 0; i < sessions.size(); ++i) {
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
    ctx.state.resume_view_offset = 0;
    ctx.state.resume_picker_active = true;

    // Capture session list for callback
    auto captured_sessions = sessions;
    auto* sm = ctx.session_manager;
    auto* al = &ctx.agent_loop;
    auto* provider_handle = ctx.provider_handle;
    auto* provider_mu = ctx.provider_mu;
    auto* config = &ctx.config;
    auto* tools = ctx.tools;
    std::string cwd = ctx.cwd;
    ctx.state.resume_callback = [&state = ctx.state, sm, al,
                                 provider_handle, provider_mu, config, tools, cwd,
                                 captured_sessions](const std::string& sid) {
        // resume 前应用 provider+model(任务 6.3)。和 do_resume_session 同源。
        const SessionMeta* target = nullptr;
        for (const auto& s : captured_sessions) {
            if (s.id == sid) { target = &s; break; }
        }
        if (target && provider_handle && provider_mu && config &&
            !target->provider.empty() && !target->model.empty()) {
            auto cwd_override = load_cwd_model_override(cwd);
            ModelProfile resumed_entry = resolve_effective_model(
                *config, cwd_override, std::optional<SessionMeta>{*target});
            swap_provider_if_needed(*provider_handle, *provider_mu, resumed_entry, *config);
            if (sm) sm->set_active_provider(
                provider_handle->get()->name(), provider_handle->get()->model());
            if (resumed_entry.name.rfind("(session:", 0) == 0) {
                state.conversation.push_back({"system",
                    "⚠ Resumed with ad-hoc model entry (session recorded " +
                    target->provider + "/" + target->model +
                    ", not in saved_models). Use /model --default <name> to pick a permanent one.",
                    false});
            }
        }

        auto messages = sm->resume_session(sid);
        al->clear_messages();
        state.conversation.clear();
        ToolExecutor fallback_tools;
        const ToolExecutor& replay_tools = tools ? *tools : fallback_tools;
        append_resumed_session_messages(messages, state, *al, replay_tools);
        std::ostringstream oss;
        oss << "Resumed session " << sid << " (" << messages.size() << " messages)";
        state.conversation.push_back({"system", oss.str(), false});
        state.chat_follow_tail = true;

        std::string restored_title;
        for (const auto& s : captured_sessions) {
            if (s.id == sid) { restored_title = s.title; break; }
        }
        if (!restored_title.empty()) {
            set_terminal_title(restored_title);
            state.current_session_title = restored_title;
        } else {
            state.current_session_title.clear();
        }
    };
}

namespace {

void clear_rewind_picker(TuiState& state) {
    state.rewind_picker_active = false;
    state.rewind_mode_active = false;
    state.rewind_items.clear();
    state.rewind_selected = 0;
    state.rewind_view_offset = 0;
    state.rewind_modes.clear();
    state.rewind_mode_selected = 0;
    state.rewind_callback = nullptr;
}

std::string format_code_restore_result(const FileCheckpointRestoreResult& result) {
    std::ostringstream oss;
    if (result.files_changed.empty()) {
        oss << "Code rewind: no tracked file changes needed.";
    } else {
        oss << "Code rewind restored " << result.files_changed.size()
            << " file" << (result.files_changed.size() == 1 ? "" : "s") << ".";
    }
    if (!result.errors.empty()) {
        oss << "\nErrors:";
        for (const auto& err : result.errors) {
            oss << "\n  - " << err;
        }
    }
    return oss.str();
}

std::string format_conversation_rewind_status(const TuiState::RewindItem& item,
                                              const std::string& new_session_id) {
    std::ostringstream oss;
    oss << "Conversation rewound to: " << item.preview;
    if (!new_session_id.empty()) {
        oss << "\nNew session: " << new_session_id
            << "\nOriginal full session remains available in /resume.";
    }
    return oss.str();
}

} // namespace

static void cmd_rewind(CommandContext& ctx, const std::string& /*args*/) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);

    if (!ctx.session_manager) {
        ctx.state.conversation.push_back({"system", "Session persistence is not available.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    if (ctx.state.is_waiting || ctx.state.tool_running || ctx.state.confirm_pending ||
        ctx.state.ask_pending || ctx.state.is_compacting) {
        ctx.state.conversation.push_back({"system",
            "Rewind is unavailable while an agent turn, tool, confirmation, question, or compaction is active.",
            false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    auto targets = collect_rewind_targets(ctx.agent_loop.messages());
    if (targets.empty()) {
        ctx.state.conversation.push_back({"system", "No user turns are available to rewind.", false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    clear_rewind_picker(ctx.state);
    // Show every collected user turn — viewport scrolling in the TUI handles
    // long lists, so no per-call cap is needed.
    ctx.state.rewind_items.reserve(targets.size());

    for (size_t shown = 0; shown < targets.size(); ++shown) {
        const auto& target = targets[targets.size() - 1 - shown];

        TuiState::RewindItem item;
        item.message_index = target.message_index;
        item.message_uuid = target.message_uuid;
        item.preview = target.preview;
        item.has_stable_uuid = target.has_stable_uuid;
        item.can_restore_code =
            item.has_stable_uuid &&
            ctx.session_manager->file_checkpoint_can_restore(item.message_uuid);

        if (item.can_restore_code) {
            auto stats = ctx.session_manager->file_checkpoint_diff_stats(item.message_uuid);
            item.changed_files = static_cast<int>(stats.files_changed.size());
            item.insertions = stats.insertions;
            item.deletions = stats.deletions;
        }

        std::ostringstream line;
        line << "[" << (shown + 1) << "] " << item.preview;
        if (item.can_restore_code) {
            line << "  code: " << item.changed_files << " file"
                 << (item.changed_files == 1 ? "" : "s")
                 << " +" << item.insertions << " -" << item.deletions;
        } else if (!item.has_stable_uuid) {
            line << "  conversation only (legacy)";
        } else {
            line << "  conversation only";
        }
        item.display = line.str();
        ctx.state.rewind_items.push_back(std::move(item));
    }

    auto* sm = ctx.session_manager;
    auto* al = &ctx.agent_loop;
    auto* tools = ctx.tools;
    auto* token_tracker = &ctx.token_tracker;
    ctx.state.rewind_callback =
        [&state = ctx.state, sm, al, tools, token_tracker](
            TuiState::RewindItem item,
            TuiState::RewindRestoreMode mode) {
            // Caller holds state.mu, matching the /resume picker callback.
            if (mode == TuiState::RewindRestoreMode::NeverMind) {
                state.conversation.push_back({"system", "Rewind cancelled.", false});
                state.chat_follow_tail = true;
                return;
            }

            const bool wants_code =
                mode == TuiState::RewindRestoreMode::CodeOnly ||
                mode == TuiState::RewindRestoreMode::CodeAndConversation;
            const bool wants_conversation =
                mode == TuiState::RewindRestoreMode::ConversationOnly ||
                mode == TuiState::RewindRestoreMode::CodeAndConversation;

            std::string code_status;
            if (wants_code) {
                if (!item.can_restore_code || item.message_uuid.empty()) {
                    code_status = "Code rewind is not available for this turn.";
                } else {
                    code_status = format_code_restore_result(
                        sm->rewind_files_to_checkpoint(item.message_uuid));
                }
            }

            if (wants_conversation) {
                auto messages = al->messages();
                if (item.message_index >= messages.size()) {
                    state.conversation.push_back({"system",
                        "Rewind failed: selected message is no longer available.", false});
                    state.chat_follow_tail = true;
                    return;
                }

                std::string prefill = rewind_prefill_text(messages[item.message_index]);
                auto retained = retained_prefix_before_index(messages, item.message_index);
                std::string new_session_id = sm->fork_active_session(retained);

                al->clear_messages();
                state.conversation.clear();
                ToolExecutor fallback_tools;
                const ToolExecutor& replay_tools = tools ? *tools : fallback_tools;
                append_resumed_session_messages(retained, state, *al, replay_tools);

                if (!code_status.empty()) {
                    state.conversation.push_back({"system", code_status, false});
                }
                state.conversation.push_back({
                    "system",
                    format_conversation_rewind_status(item, new_session_id),
                    false});

                state.input_mode = InputMode::Normal;
                state.input_text = std::move(prefill);
                state.input_cursor = state.input_text.size();
                state.history_index = -1;
                state.pending_queue.clear();
                state.token_status.clear();
                token_tracker->reset();
                state.chat_follow_tail = true;
                state.chat_focus_index = static_cast<int>(state.conversation.size()) - 1;
                state.chat_line_offset = 0;
                return;
            }

            if (!code_status.empty()) {
                state.conversation.push_back({"system", code_status, false});
                state.chat_follow_tail = true;
            }
        };

    ctx.state.rewind_selected = 0;
    ctx.state.rewind_mode_selected = 0;
    ctx.state.rewind_picker_active = true;
    ctx.state.rewind_mode_active = false;
}

void register_builtin_commands(CommandRegistry& registry) {
    registry.register_command({"help", "Show available commands", cmd_help});
    registry.register_command({"clear", "Clear conversation history", cmd_clear});
    registry.register_command({"model", "Show or switch current model", cmd_model});
    registry.register_command({"config", "Show current configuration", cmd_config});
    registry.register_command({"tokens", "Show session token usage", cmd_tokens});
    registry.register_command({"compact", "Compress conversation history", cmd_compact});
    registry.register_command({"resume", "Resume a previous session", cmd_resume});
    registry.register_command({"rewind", "Rewind to a previous user turn", cmd_rewind});
    registry.register_command({"checkpoint", "Alias for /rewind", cmd_rewind});
    registry.register_command({"mcp", "Manage MCP servers", cmd_mcp});
    registry.register_command({"skills", "List, invoke, or reload installed skills", cmd_skills});
    register_memory_command(registry);
    register_init_command(registry);
    register_history_command(registry);
    register_proxy_command(registry);
    register_websearch_command(registry);
    registry.register_command({"title", "Set or show the window title for this session", cmd_title});
    registry.register_command({"exit", "Exit acecode", cmd_exit});
    register_models_command(registry);
}

} // namespace acecode
