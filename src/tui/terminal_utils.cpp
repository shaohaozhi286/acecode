#include "tui/terminal_utils.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <termios.h>
#include <unistd.h>
#include <csignal>
#endif

#include "version.hpp"
#include "config/config.hpp"
#include "utils/logger.hpp"
#include "utils/terminal_title.hpp"
#include "skills/default_skill_seeder.hpp"
#include "session/session_manager.hpp"
#include "tui/chat_scroll.hpp"

namespace acecode { namespace tui {

std::string get_cwd() {
#ifdef _WIN32
    char buf[MAX_PATH];
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return ".";
}

std::string get_executable_dir_from_argv(int argc, char* argv[]) {
    if (argc <= 0 || !argv[0]) return "";
    std::error_code ec;
    std::filesystem::path exe(argv[0]);
    std::filesystem::path abs = std::filesystem::weakly_canonical(exe, ec);
    if (!ec) return abs.parent_path().string();
    return exe.parent_path().string();
}

void seed_default_skills_if_first_initialization(const std::string& argv0_dir) {
    bool first_initialization = acecode::consume_acecode_home_created_by_process();
    auto result = acecode::install_default_global_skills_on_first_initialization(
        std::filesystem::path(acecode::get_acecode_dir()),
        argv0_dir, first_initialization);
    if (!result.attempted) return;
    size_t installed = 0, skipped = 0, errors = 0;
    for (const auto& outcome : result.outcomes) {
        if (outcome.result == "installed") ++installed;
        else if (outcome.result == "skipped") ++skipped;
        else ++errors;
    }
    if (!result.error.empty()) {
        LOG_WARN("[skills] Default skill seeding issue: " + result.error);
    }
    LOG_INFO("[skills] Default skill seeding attempted: installed=" +
             std::to_string(installed) + " skipped=" + std::to_string(skipped) +
             " errors=" + std::to_string(errors));
}

void write_terminal_control_sequence(std::string_view seq) {
#ifdef _WIN32
    auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode = 0;
    const bool restore_mode =
        stdout_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(stdout_handle, &out_mode);
    if (!restore_mode) return;
    constexpr DWORD enable_virtual_terminal_processing = 0x0004;
    constexpr DWORD disable_newline_auto_return = 0x0008;
    SetConsoleMode(stdout_handle,
                   out_mode | enable_virtual_terminal_processing |
                       disable_newline_auto_return);
#endif
    std::cout.write(seq.data(), static_cast<std::streamsize>(seq.size()));
    std::cout.flush();
#ifdef _WIN32
    SetConsoleMode(stdout_handle, out_mode);
#endif
}

void set_ftxui_full_repaint_mode(bool enabled) {
#ifdef _WIN32
    _putenv_s("ACECODE_FTXUI_FULL_REPAINT", enabled ? "1" : "0");
#else
    if (enabled) setenv("ACECODE_FTXUI_FULL_REPAINT", "1", 1);
    else unsetenv("ACECODE_FTXUI_FULL_REPAINT");
#endif
}

void reset_cursor() {
    write_terminal_control_sequence("\033[?25h");
}

void flush_terminal_input_buffer() {
#ifdef _WIN32
    auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (stdin_handle == INVALID_HANDLE_VALUE) return;
    FlushConsoleInputBuffer(stdin_handle);
#else
    if (isatty(STDIN_FILENO)) tcflush(STDIN_FILENO, TCIFLUSH);
#endif
}

// Globals
SessionManager* g_session_manager = nullptr;
std::atomic<ftxui::ScreenInteractive*> g_active_screen{nullptr};

void finalize_session_atexit() {
    if (g_session_manager) {
        g_session_manager->finalize();
        auto sid = g_session_manager->current_session_id();
        if (!sid.empty()) {
            std::cerr << "\nacecode: session " << sid
                      << " saved. Resume with: acecode --resume " << sid << std::endl;
        }
    }
    clear_terminal_title();
}

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    auto* s = g_active_screen.load(std::memory_order_acquire);
    if (ctrl_type == CTRL_C_EVENT) {
        if (s) { s->PostEvent(ftxui::Event::CtrlC); return TRUE; }
        finalize_session_atexit();
        return FALSE;
    }
    if (ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        if (s) { s->Exit(); return TRUE; }
        finalize_session_atexit();
        return FALSE;
    }
    return FALSE;
}

void prepare_windows_ctrl_c_handling_after_ftxui_install() {
    auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = 0;
    if (stdin_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(stdin_handle, &in_mode)) {
        SetConsoleMode(stdin_handle, in_mode & ~ENABLE_PROCESSED_INPUT);
    }
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}
#else
void signal_handler(int /*sig*/) {
    finalize_session_atexit();
    _exit(1);
}
#endif

}} // namespace acecode::tui
