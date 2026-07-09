#pragma once

#include "../tui_state.hpp"
#include "../agent_loop.hpp"
#include "../provider/llm_provider.hpp"
#include "../config/config.hpp"
#include "../utils/token_tracker.hpp"
#include "../session/session_manager.hpp"
#include "../session/session_registry.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <utility>

namespace acecode {

class McpManager;
class ToolExecutor;
class SkillRegistry;
class MemoryRegistry;
class CommandRegistry;
namespace tui { class SubagentHost; }

struct CommandContext {
    TuiState& state;
    AgentLoop& agent_loop;
    // ProviderSlot 持有当前 LlmProvider + mutex。读 provider 字段时,从 slot 拿
    // shared_ptr 副本(`auto p = ctx.provider_slot->provider;`),保活引用不被
    // 并发 swap 拽走;写(切模型)用 apply_model_to_session。
    SessionEntry::ProviderSlot* provider_slot = nullptr;
    AppConfig& config;
    TokenTracker& token_tracker;
    PermissionManager& permissions;
    std::function<void()> request_exit;
    SessionManager* session_manager = nullptr;
    std::function<void()> post_event;  // post a TUI refresh event from any thread
    McpManager* mcp_manager = nullptr; // runtime MCP control surface (optional)
    ToolExecutor* tools = nullptr;     // tool registry for /mcp enable/disable
    SkillRegistry* skills = nullptr;   // skill registry for /skills and /<skill-name> commands
    MemoryRegistry* memory = nullptr;  // memory registry for /memory commands
    CommandRegistry* command_registry = nullptr; // self-reference for /skills reload
    std::string cwd;                   // working directory for cwd-scoped operations
    tui::SubagentHost* subagent_host = nullptr; // /tasks 的子代理宿主(仅斜杠 dispatch 路径注入)
    std::function<void(const UserInput&)> submit_user_input; // optional TUI submit wrapper
};

inline void submit_user_input(CommandContext& ctx, UserInput input) {
    if (ctx.submit_user_input) {
        ctx.submit_user_input(input);
    } else {
        ctx.agent_loop.submit(input);
    }
}

inline void submit_user_text(CommandContext& ctx,
                             const std::string& text,
                             const std::string& display_text = std::string{}) {
    UserInput input;
    input.text = text;
    input.display_text = display_text;
    submit_user_input(ctx, std::move(input));
}

struct SlashCommand {
    std::string name;
    std::string description;
    std::function<void(CommandContext& ctx, const std::string& args)> execute;
};

class CommandRegistry {
public:
    void register_command(const SlashCommand& cmd);

    // Remove a single command by name. Returns true when it existed.
    bool unregister_command(const std::string& name);

    // Check whether a command is registered.
    bool has_command(const std::string& name) const {
        return commands_.find(name) != commands_.end();
    }

    // Dispatch a slash command string (e.g., "/help" or "/model gpt-4").
    // Returns true if a command was found and executed, false if unknown.
    bool dispatch(const std::string& input, CommandContext& ctx);

    // Get all registered commands (for /help)
    const std::map<std::string, SlashCommand>& commands() const { return commands_; }

private:
    std::map<std::string, SlashCommand> commands_;
};

} // namespace acecode
