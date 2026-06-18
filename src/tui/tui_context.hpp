#pragma once
// TuiContext: bundles all shared state that the TUI event handler, renderer,
// and agent callbacks need. Replaces the 30+ variable lambda captures that
// previously lived inside run_interactive_app().

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "tui_state.hpp"
#include "agent_loop.hpp"
#include "config/config.hpp"
#include "permissions.hpp"
#include "utils/token_tracker.hpp"
#include "session/session_manager.hpp"
#include "tool/tool_executor.hpp"
#include "tool/mcp_manager.hpp"
#include "skills/skill_registry.hpp"
#include "memory/memory_registry.hpp"
#include "commands/command_registry.hpp"
#include "provider/llm_provider.hpp"

namespace acecode {

// Forward declarations
struct InteractiveCliOptions;

struct TuiContext {
    // ---- Core objects ----
    TuiState& state;
    ftxui::ScreenInteractive& screen;
    AgentLoop& agent_loop;
    AppConfig& config;
    TokenTracker& token_tracker;
    PermissionManager& permissions;
    SessionManager& session_manager;
    McpManager& mcp_manager;
    ToolExecutor& tools;
    SkillRegistry& skill_registry;
    MemoryRegistry& memory_registry;
    CommandRegistry& cmd_registry;
    SessionEntry::ProviderSlot& provider_slot;
    std::function<std::shared_ptr<LlmProvider>()> provider_accessor;

    // ---- Synchronization ----
    std::atomic<bool>& auth_done;
    std::atomic<bool>& agent_aborting;
    std::atomic<bool>& mcp_first_turn_wait_done;
    std::atomic<bool>& running;
    std::atomic<int>& anim_tick;

    // ---- Layout geometry (written by Renderer, read by event handler) ----
    ftxui::Box& chat_box;
    ftxui::Box& scrollbar_box;
    ftxui::Box& ask_scrollbar_box;
    ftxui::Box& ask_overlay_box;
    std::vector<ftxui::Box>& message_boxes;
    std::vector<ftxui::Box>& message_layout_boxes;
    std::vector<int>& message_line_counts;
    int& message_line_count_width;

    // ---- Config values ----
    std::string working_dir;
    std::string version_str;
    std::string cwd_display;
    bool dangerous_mode = false;
    bool conhost_compat_layout = false;

    // ---- Scroll/viewport helper methods (were lambdas) ----
    int chat_viewport_rows() const;
    void sync_chat_line_counts_from_layout();
    void clamp_chat_focus();
    int scroll_chat_by_lines(int delta_lines);

    // ---- MCP coordination (was lambda) ----
    void coordinate_mcp_before_first_turn();

    // ---- Clipboard / paste helpers (were lambdas) ----
    void cancel_ctrl_c_exit_locked();
    void insert_pasted_text_at_cursor(const std::string& normalized);
    bool can_accept_clipboard_paste_locked() const;
    bool paste_system_clipboard_text();
    bool paste_system_clipboard_image();
    bool handle_pending_attachment_focus_event(const ftxui::Event& event);
};

} // namespace acecode
