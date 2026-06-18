#pragma once
// Terminal control utilities extracted from main.cpp.
// Sequences, cursor, input buffer, signal handlers, session finalization.

#include <string_view>
#include <atomic>
#include <ftxui/component/screen_interactive.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acecode {
class SessionManager;
}

namespace acecode { namespace tui {

// Current working directory
std::string get_cwd();
std::string get_executable_dir_from_argv(int argc, char* argv[]);

// Seed default skills on first initialization
void seed_default_skills_if_first_initialization(const std::string& argv0_dir);

// Terminal control sequences
void write_terminal_control_sequence(std::string_view seq);
void set_ftxui_full_repaint_mode(bool enabled);
void reset_cursor();
void flush_terminal_input_buffer();

// Session finalization globals
extern SessionManager* g_session_manager;
extern std::atomic<ftxui::ScreenInteractive*> g_active_screen;
void finalize_session_atexit();

// Signal handlers
#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type);
void prepare_windows_ctrl_c_handling_after_ftxui_install();
#else
void signal_handler(int sig);
#endif

}} // namespace acecode::tui
