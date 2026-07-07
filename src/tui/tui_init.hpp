#pragma once
// TUI startup initialization functions extracted from main.cpp.

#include <string>
#include <thread>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>

#include "tui/tui_context.hpp"
#include "tui/render_mode_factory.hpp"
#include "utils/terminal_capability.hpp"

namespace acecode { namespace tui {

// Terminal checks
bool ensure_interactive_terminal();
void set_startup_terminal_title();

// Runtime initialization
void initialize_logger_for_working_dir(const std::string& working_dir);
void initialize_proxy_runtime(const AppConfig& config);
void initialize_models_registry_runtime(const AppConfig& config,
                                         const std::string& argv0_dir);
void initialize_web_search_runtime(const AppConfig& config);

// Update check
std::thread start_tui_update_check(const AppConfig& config,
                                   TuiState& state,
                                   ftxui::ScreenInteractive& screen);

// Memory, MCP, input history
MemoryConfig initialize_memory_registry(MemoryRegistry& memory_registry,
                                        const AppConfig& config);
void initialize_mcp_servers(McpManager& mcp_manager, const AppConfig& config);
void restore_input_history(TuiState& state, const AppConfig& config,
                           const std::string& working_dir);
void add_startup_messages(TuiState& state, bool dangerous_mode,
                          const McpManager& mcp_manager);
void start_mcp_servers_async(McpManager& mcp_manager, ToolExecutor& tools,
                             TuiState& state,
                             ftxui::ScreenInteractive& screen);

// Terminal hint
void maybe_add_legacy_terminal_hint(
    TuiState& state, const AppConfig& config,
    const acecode::TerminalCapabilities& term_caps,
    acecode::tui::ScreenRenderMode render_mode,
    bool force_alt_screen);

// Permissions
PermissionMode permission_mode_from_meta_name(std::string mode);
void configure_permissions(PermissionManager& permissions,
                           bool dangerous_mode,
                           const std::string& default_permission_mode);

// Slash commands
void register_slash_commands(CommandRegistry& cmd_registry,
                             SkillRegistry& skill_registry,
                             const AppConfig& config,
                             const std::string& working_dir);

// TUI loop
void run_tui_loop(ftxui::ScreenInteractive& screen,
                  const ftxui::Component& renderer);

// Shutdown
void shutdown_after_tui_loop(TuiState& state, AgentLoop& agent_loop,
                             McpManager& mcp_manager,
                             std::atomic<bool>& running,
                             std::atomic<bool>& agent_aborting,
                             std::thread& anim_thread,
                             std::thread& auth_thread,
                             std::thread& update_check_thread,
                             SessionManager& session_manager,
                             const AppConfig& config);

}} // namespace acecode::tui
