#pragma once
// TUI helper functions extracted from the anonymous namespace in main.cpp.
// Thinking phrases, text truncation, sidebar rendering, status bar chips,
// MCP sidebar, input text wrapping, and related utilities.

#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <cstddef>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>

#include "tui_state.hpp"
#include "tool/mcp_manager.hpp"

namespace acecode { namespace tui {

// ---- Thinking phrases ----
bool is_user_chinese(const TuiState& state);
std::string get_random_thinking_phrase(bool is_zh);

// ---- Text helpers ----
bool is_success_summary(const ToolSummary& s);
std::string renderable_tool_summary_line(const ToolSummary& s,
                                         const std::string& metric_str,
                                         int max_visual_width);
std::string collapse_sidebar_title_whitespace(std::string_view text);
void trim_ascii_space_suffix(std::string& text);
std::string truncate_cells_prefix(std::string_view text, int max_cells);
std::string truncate_cells_middle_ascii(std::string_view text, int max_cells);
std::string format_tool_count(size_t tool_count);
std::string uppercase_ascii(std::string text);
std::string repeat_utf8_glyph(const char* glyph, int count);

// ---- Sidebar rendering ----
// Single composition entry point for the regular TUI sidebar. Keep sections
// here so main.cpp cannot drift from shared sidebar behavior.
ftxui::Element render_regular_sidebar(const TuiState& state,
                                      const std::string& version_str,
                                      const std::string& cwd_display,
                                      int sidebar_width, int anim_tick);
ftxui::Element render_pending_queue_block(const TuiState& state, int available_width);
ftxui::Element render_pending_attachment_block(const TuiState& state, int available_width);

// ---- Status bar ----
ftxui::Color token_progress_color(int percent);
ftxui::Color model_load_color(int percent);
ftxui::Element render_model_load_chip();
ftxui::Color status_line_color(const std::string& status_line);
ftxui::Element render_token_usage_chip(const TuiState& state);
ftxui::Element queued_badge();

// ---- MCP sidebar ----
std::string mcp_state_label(McpServerState state);
ftxui::Color mcp_sidebar_state_color(const std::string& state);
bool mcp_sidebar_has_loading(const std::vector<TuiState::McpSidebarServer>& servers);
bool mcp_sidebar_has_loading(const TuiState& state);
ftxui::Element render_white_shimmer_text(const std::string& label, int anim_tick, bool with_dots = true);
std::vector<TuiState::McpSidebarServer> build_mcp_sidebar_servers(const McpManager& manager);
void set_mcp_sidebar_servers_locked(TuiState& state, std::vector<TuiState::McpSidebarServer> servers);

// ---- Tool result ----
ftxui::Element render_tool_result_lines_preserving_breaks(const std::string& display_content);

// ---- Input text wrapping ----
bool is_space_glyph(const std::string& glyph);
bool is_narrow_glyph(const std::string& glyph);
bool is_opening_cjk_punctuation(const std::string& glyph);
bool is_closing_cjk_punctuation(const std::string& glyph);
void flush_ascii_run(std::string* ascii_run, std::string* pending_prefix, std::vector<std::string>* output);
std::vector<std::string> tokenize_wrapped_input(const std::string& text);
ftxui::Element render_wrapped_input_text(const std::string& input_value, size_t cursor_bytes);

// ---- Model load global ----
extern std::atomic<int> g_model_load_percent;

}} // namespace acecode::tui
