#pragma once

#include "../tui_state.hpp"
#include "../agent_loop.hpp"
#include "../provider/llm_provider.hpp"
#include "../config/config.hpp"
#include "../utils/token_tracker.hpp"
#include "../session/session_manager.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <map>
#include <vector>
#include <functional>

namespace acecode {

class McpManager;
class ToolExecutor;
class SkillRegistry;
class MemoryRegistry;
class CommandRegistry;

struct CommandContext {
    TuiState& state;
    AgentLoop& agent_loop;
    // Snapshot of the current provider for read-only operations within the
    // command. The shared_ptr keeps the instance alive across the command body
    // even if main.cpp swaps `provider` mid-call. For commands that need to
    // *swap* the provider (e.g. /model), use `provider_handle` + `provider_mu`.
    LlmProvider& provider;
    // Live handle into main.cpp's `std::shared_ptr<LlmProvider> provider`.
    // Required by /model so it can call swap_provider_if_needed under lock.
    std::shared_ptr<LlmProvider>* provider_handle = nullptr;
    std::mutex* provider_mu = nullptr;
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
};

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
