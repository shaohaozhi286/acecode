#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <imm.h>
#pragma comment(lib, "Imm32.lib")
#else
#include <unistd.h>
#endif

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#ifdef _WIN32
#include <ftxui/screen/string.hpp>
#endif

#include "version.hpp"
#include "config/config.hpp"
#include "provider/provider_factory.hpp"
#include "provider/copilot_provider.hpp"
#include "tool/tool_executor.hpp"
#include "tool/bash_tool.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/file_edit_tool.hpp"
#include "tool/grep_tool.hpp"
#include "tool/glob_tool.hpp"
#include "utils/logger.hpp"
#include "permissions.hpp"
#include "agent_loop.hpp"
#include "commands/configure.hpp"

using namespace ftxui;
using namespace acecode;

// ---- Get current working directory ----
static std::string get_cwd() {
#ifdef _WIN32
    char buf[MAX_PATH];
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return ".";
}

// ---- Reset terminal cursor visibility on exit ----
static void reset_cursor() {
    // DECTCEM: show cursor (ESC [ ? 25 h)
    std::cout << "\033[?25h" << std::flush;
}

#ifdef _WIN32
static int max_int(int a, int b) {
    return a > b ? a : b;
}

static std::string ptr_to_hex(const void* ptr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << reinterpret_cast<uintptr_t>(ptr);
    return oss.str();
}

static std::string dword_to_hex(DWORD value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << value;
    return oss.str();
}

static int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static int display_width_utf8(const std::string& text) {
    return max_int(0, ftxui::string_width(text));
}

static HWND get_ime_target_window(HWND fallback_hwnd) {
    HWND target = GetForegroundWindow();
    if (!target) {
        LOG_DEBUG("IME: GetForegroundWindow returned null, fallback=" + ptr_to_hex(fallback_hwnd));
        return fallback_hwnd;
    }

    LOG_DEBUG("IME: foreground window=" + ptr_to_hex(target));

    GUITHREADINFO gui_thread_info{};
    gui_thread_info.cbSize = sizeof(gui_thread_info);

    DWORD thread_id = GetWindowThreadProcessId(target, nullptr);
    if (thread_id != 0 && GetGUIThreadInfo(thread_id, &gui_thread_info) && gui_thread_info.hwndFocus) {
        LOG_DEBUG("IME: GUI thread focus window=" + ptr_to_hex(gui_thread_info.hwndFocus) +
                  ", active=" + ptr_to_hex(gui_thread_info.hwndActive) +
                  ", capture=" + ptr_to_hex(gui_thread_info.hwndCapture));
        return gui_thread_info.hwndFocus;
    }

    LOG_DEBUG("IME: GetGUIThreadInfo unavailable, thread_id=" + std::to_string(thread_id) +
              ", last_error=" + dword_to_hex(GetLastError()) +
              ", using foreground window");

    return target;
}

static void update_ime_composition_window(const std::string& input_text,
                                          bool show_bottom_bar,
                                          bool confirm_pending,
                                          const std::string& confirm_tool_name) {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) {
        LOG_WARN("IME: GetConsoleWindow returned null");
        return;
    }

    HANDLE hconsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hconsole == INVALID_HANDLE_VALUE) {
        LOG_WARN("IME: GetStdHandle(STD_OUTPUT_HANDLE) failed, last_error=" + dword_to_hex(GetLastError()));
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(hconsole, &csbi)) {
        LOG_WARN("IME: GetConsoleScreenBufferInfo failed, last_error=" + dword_to_hex(GetLastError()));
        return;
    }

    const int visible_cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    const int visible_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (visible_cols <= 0 || visible_rows <= 0) {
        LOG_WARN("IME: invalid visible size, cols=" + std::to_string(visible_cols) +
                 ", rows=" + std::to_string(visible_rows));
        return;
    }

    // GetClientRect(GetConsoleWindow()) 在 Windows Terminal / VS 集成终端 / ConPTY 下
    // 经常拿到的是一个伪控制台窗口，client rect 可能恒为 0。
    // 这里优先使用控制台字体尺寸来计算单元格像素大小，不依赖 client rect。
    int cell_width = 0;
    int cell_height = 0;

    CONSOLE_FONT_INFOEX cfi{};
    cfi.cbSize = sizeof(cfi);
    if (GetCurrentConsoleFontEx(hconsole, FALSE, &cfi)) {
        cell_width = max_int(1, static_cast<int>(cfi.dwFontSize.X));
        cell_height = max_int(1, static_cast<int>(cfi.dwFontSize.Y));
        LOG_DEBUG("IME: font metrics width=" + std::to_string(cell_width) +
                  ", height=" + std::to_string(cell_height));
    } else {
        RECT client{};
        if (!GetClientRect(hwnd, &client)) {
            LOG_WARN("IME: GetCurrentConsoleFontEx and GetClientRect both failed, font_error=" +
                     dword_to_hex(GetLastError()));
            return;
        }

        const int client_width = client.right - client.left;
        const int client_height = client.bottom - client.top;
        if (client_width <= 0 || client_height <= 0) {
            LOG_WARN("IME: invalid client size, width=" + std::to_string(client_width) +
                     ", height=" + std::to_string(client_height));
            return;
        }

        cell_width = max_int(1, client_width / visible_cols);
        cell_height = max_int(1, client_height / visible_rows);
        LOG_DEBUG("IME: fallback client metrics width=" + std::to_string(cell_width) +
                  ", height=" + std::to_string(cell_height));
    }

    const int border_padding = 2;
    const int prefix_width = confirm_pending
        ? display_width_utf8(" [" + confirm_tool_name + "] ") + display_width_utf8("yes / always / no: ")
        : display_width_utf8(" > ");
    const int available_cols = max_int(1, visible_cols - border_padding - prefix_width);

    const int input_width = display_width_utf8(input_text);
    const int wrapped_col = input_width % available_cols;
    const int wrapped_row = input_width / available_cols;

    const int prompt_bottom_row = max_int(0, visible_rows - 2 - (show_bottom_bar ? 1 : 0));
    const int caret_col = clamp_int(1 + prefix_width + wrapped_col, 0, visible_cols - 1);
    const int caret_row = clamp_int(prompt_bottom_row + wrapped_row, 0, visible_rows - 1);

    LOG_DEBUG("IME: input='" + log_truncate(input_text, 120) +
              "', confirm_pending=" + std::string(confirm_pending ? "true" : "false") +
              ", show_bottom_bar=" + std::string(show_bottom_bar ? "true" : "false") +
              ", console_hwnd=" + ptr_to_hex(hwnd) +
              ", visible_cols=" + std::to_string(visible_cols) +
              ", visible_rows=" + std::to_string(visible_rows) +
              ", prefix_width=" + std::to_string(prefix_width) +
              ", available_cols=" + std::to_string(available_cols) +
              ", input_width=" + std::to_string(input_width) +
              ", wrapped_col=" + std::to_string(wrapped_col) +
              ", wrapped_row=" + std::to_string(wrapped_row) +
              ", caret_col=" + std::to_string(caret_col) +
              ", caret_row=" + std::to_string(caret_row) +
              ", pixel_x=" + std::to_string(caret_col * cell_width) +
              ", pixel_y=" + std::to_string(caret_row * cell_height));

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_FORCE_POSITION;
    composition.ptCurrentPos.x = caret_col * cell_width;
    composition.ptCurrentPos.y = caret_row * cell_height;

    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos.x = composition.ptCurrentPos.x;
    candidate.ptCurrentPos.y = composition.ptCurrentPos.y + cell_height;

    HWND ime_target = get_ime_target_window(hwnd);
    HIMC himc = ImmGetContext(ime_target);
    if (himc) {
        const BOOL composition_ok = ImmSetCompositionWindow(himc, &composition);
        const DWORD composition_error = GetLastError();
        const BOOL candidate_ok = ImmSetCandidateWindow(himc, &candidate);
        const DWORD candidate_error = GetLastError();
        LOG_DEBUG("IME: ImmGetContext success, target=" + ptr_to_hex(ime_target) +
                  ", himc=" + ptr_to_hex(himc) +
                  ", ImmSetCompositionWindow=" + std::to_string(composition_ok) +
                  ", comp_error=" + dword_to_hex(composition_error) +
                  ", ImmSetCandidateWindow=" + std::to_string(candidate_ok) +
                  ", cand_error=" + dword_to_hex(candidate_error));
        ImmReleaseContext(ime_target, himc);
        return;
    }

    LOG_WARN("IME: ImmGetContext returned null, target=" + ptr_to_hex(ime_target) +
             ", last_error=" + dword_to_hex(GetLastError()));

    HWND default_ime_window = ImmGetDefaultIMEWnd(ime_target);
    if (!default_ime_window) {
        LOG_WARN("IME: ImmGetDefaultIMEWnd returned null for target=" + ptr_to_hex(ime_target));
        return;
    }

    const LRESULT composition_result = SendMessage(default_ime_window,
                                                   WM_IME_CONTROL,
                                                   IMC_SETCOMPOSITIONWINDOW,
                                                   reinterpret_cast<LPARAM>(&composition));
    const DWORD composition_send_error = GetLastError();
    const LRESULT candidate_result = SendMessage(default_ime_window,
                                                 WM_IME_CONTROL,
                                                 IMC_SETCANDIDATEPOS,
                                                 reinterpret_cast<LPARAM>(&candidate));
    const DWORD candidate_send_error = GetLastError();
    LOG_DEBUG("IME: default IME window=" + ptr_to_hex(default_ime_window) +
              ", SendMessage(comp)=" + std::to_string(static_cast<long long>(composition_result)) +
              ", comp_error=" + dword_to_hex(composition_send_error) +
              ", SendMessage(cand)=" + std::to_string(static_cast<long long>(candidate_result)) +
              ", cand_error=" + dword_to_hex(candidate_send_error));
}
#endif

// ---- Shared TUI state ----
struct TuiState {
    struct Message {
        std::string role;
        std::string content;
        bool is_tool = false;
    };

    std::vector<Message> conversation;
    std::string input_text;
    bool is_waiting = false;
    std::string status_line; // for auth/provider status

    // Input history for up/down navigation
    std::vector<std::string> input_history;
    int history_index = -1; // -1 = not browsing history
    std::string saved_input; // saved current input when entering history

    // Pending message queue
    std::vector<std::string> pending_queue;

    // Tool confirmation state
    bool confirm_pending = false;
    std::string confirm_tool_name;
    std::string confirm_tool_args;
    PermissionResult confirm_result = PermissionResult::Deny;
    std::condition_variable confirm_cv;

    int chat_focus_index = -1;
    bool chat_follow_tail = true;
    bool ctrl_c_armed = false;
    std::chrono::steady_clock::time_point last_ctrl_c_time{};

    std::mutex mu;
};

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // SECURITY: Prevent Windows from executing commands from current directory.
    // Without this, a malicious exe placed in cwd could hijack system commands.
    SetEnvironmentVariableA("NoDefaultCurrentDirectoryInExePath", "1");
#endif

    // ---- Parse CLI arguments ----
    bool dangerous_mode = false;
    bool run_configure_cmd = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-dangerous" || arg == "--dangerous") {
            dangerous_mode = true;
        } else if (arg == "configure") {
            run_configure_cmd = true;
        }
    }

    // ---- Handle configure subcommand (before TUI setup) ----
    if (run_configure_cmd) {
        AppConfig config = load_config();
        return run_configure(config);
    }

    // ---- Ensure cursor is restored on exit ----
    std::atexit(reset_cursor);

    // ---- Check stdin/stdout are TTYs (interactive terminal) ----
#ifdef _WIN32
    bool stdin_is_tty = _isatty(_fileno(stdin));
    bool stdout_is_tty = _isatty(_fileno(stdout));
#else
    bool stdin_is_tty = isatty(fileno(stdin));
    bool stdout_is_tty = isatty(fileno(stdout));
#endif
    if (!stdin_is_tty || !stdout_is_tty) {
        std::cerr << "Error: acecode requires an interactive terminal (stdin and stdout must be a TTY).\n"
                  << "If piping input/output, please run acecode directly in a terminal instead.\n";
        return 1;
    }

    // ---- Set terminal title ----
#ifdef _WIN32
    SetConsoleTitleA("acecode v" ACECODE_VERSION);
#else
    // xterm-compatible title escape sequence
    std::cout << "\033]0;acecode v" ACECODE_VERSION "\007" << std::flush;
#endif

    // ---- Record working directory ----
    std::string working_dir = get_cwd();

    // ---- Init logger ----
    Logger::instance().init(working_dir + "/acecode.log");
    Logger::instance().set_level(LogLevel::Dbg);
    LOG_INFO("=== acecode started, cwd=" + working_dir + " ===");

    // ---- Load config ----
    AppConfig config = load_config();

    // ---- Create provider ----
    auto provider = create_provider(config);

    // ---- Setup tools ----
    ToolExecutor tools;
    tools.register_tool(create_bash_tool());
    tools.register_tool(create_file_read_tool());
    tools.register_tool(create_file_write_tool());
    tools.register_tool(create_file_edit_tool());
    tools.register_tool(create_grep_tool());
    tools.register_tool(create_glob_tool());

    // ---- TUI state ----
    TuiState state;
    state.status_line = "[" + provider->name() + "] model: " +
        (config.provider == "copilot" ? config.copilot.model : config.openai.model);

    // Version and working directory strings for TUI header
    std::string version_str = "acecode v" ACECODE_VERSION;
    std::string cwd_display = working_dir;

    // If dangerous mode, show startup warning
    if (dangerous_mode) {
        state.conversation.push_back({"system",
            "[DANGEROUS MODE] All permission checks are bypassed. Use with caution!", false});
    }

    // Animation tick for Thinking... indicator
    std::atomic<int> anim_tick{0};
    Box chat_box;

    auto screen = ScreenInteractive::TerminalOutput();
    screen.TrackMouse(false);
    screen.ForceHandleCtrlC(false);

    auto clamp_chat_focus = [&]() {
        if (state.conversation.empty()) {
            state.chat_focus_index = -1;
            state.chat_follow_tail = true;
            return;
        }

        int last = static_cast<int>(state.conversation.size()) - 1;
        if (state.chat_follow_tail) {
            state.chat_focus_index = last;
            return;
        }

        if (state.chat_focus_index < 0) {
            state.chat_focus_index = 0;
        }
        if (state.chat_focus_index > last) {
            state.chat_focus_index = last;
        }
        if (state.chat_focus_index == last) {
            state.chat_follow_tail = true;
        }
    };

    auto scroll_chat = [&](int delta) -> bool {
        if (state.conversation.empty()) {
            return false;
        }

        int last = static_cast<int>(state.conversation.size()) - 1;
        int current = state.chat_follow_tail ? last : state.chat_focus_index;
        if (current < 0) {
            current = last;
        }

        int next = current + delta;
        if (next < 0) {
            next = 0;
        }
        if (next > last) {
            next = last;
        }

        state.chat_focus_index = next;
        state.chat_follow_tail = (next == last);
        return next != current;
    };

    // ---- Copilot auth flow (background thread) ----
    std::atomic<bool> auth_done{false};
    std::thread auth_thread;

    if (config.provider == "copilot") {
        auto* copilot = dynamic_cast<CopilotProvider*>(provider.get());
        if (copilot && !copilot->is_authenticated()) {
            {
                std::lock_guard<std::mutex> lk(state.mu);
                state.is_waiting = true;
                state.conversation.push_back({"system", "Authenticating with GitHub Copilot...", false});
            }
            screen.PostEvent(Event::Custom);

            auth_thread = std::thread([&] {
                // Try silent auth first (saved token)
                if (copilot->try_silent_auth()) {
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        state.conversation.push_back({"system", "Authenticated (saved token).", false});
                        state.is_waiting = false;
                    }
                    auth_done = true;
                    screen.PostEvent(Event::Custom);
                    return;
                }

                // Need interactive device flow
                auto dc = request_device_code();
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.conversation.push_back({"system",
                        "Open " + dc.verification_uri + " and enter code: " + dc.user_code, false});
                }
                screen.PostEvent(Event::Custom);

                copilot->run_device_flow([&](const std::string& status) {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.status_line = status;
                    screen.PostEvent(Event::Custom);
                });

                if (copilot->is_authenticated()) {
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        state.conversation.push_back({"system", "GitHub Copilot authenticated!", false});
                        state.is_waiting = false;
                        state.status_line = "[copilot] model: " + config.copilot.model;
                    }
                } else {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.conversation.push_back({"system", "[Error] Authentication failed.", false});
                    state.is_waiting = false;
                }
                auth_done = true;
                screen.PostEvent(Event::Custom);
            });
        } else {
            auth_done = true;
        }
    } else {
        auth_done = true;
    }

    // ---- Agent callbacks ----
    AgentCallbacks callbacks;
    callbacks.on_message = [&](const std::string& role, const std::string& content, bool is_tool) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!is_tool && role == "assistant" &&
            !state.conversation.empty() &&
            state.conversation.back().role == "assistant" &&
            !state.conversation.back().is_tool) {
            state.conversation.back().content = content;
        } else {
            state.conversation.push_back({role, content, is_tool});
        }
        clamp_chat_focus();
        screen.PostEvent(Event::Custom);
    };
    callbacks.on_busy_changed = [&](bool busy) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.is_waiting = busy;
        screen.PostEvent(Event::Custom);
    };
    callbacks.on_tool_confirm = [&](const std::string& tool_name, const std::string& args) -> PermissionResult {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.confirm_pending = true;
            state.confirm_tool_name = tool_name;
            state.confirm_tool_args = args;
        }
        screen.PostEvent(Event::Custom);

        // Block the agent thread until the user responds in the TUI
        std::unique_lock<std::mutex> lk(state.mu);
        state.confirm_cv.wait(lk, [&] { return !state.confirm_pending; });
        return state.confirm_result;
    };
    callbacks.on_delta = [&](const std::string& token) {
        std::lock_guard<std::mutex> lk(state.mu);
        // Find or create the streaming assistant message
        if (state.conversation.empty() ||
            state.conversation.back().role != "assistant" ||
            state.conversation.back().is_tool) {
            state.conversation.push_back({"assistant", "", false});
        }
        state.conversation.back().content += token;
        clamp_chat_focus();
        screen.PostEvent(Event::Custom);
    };

    PermissionManager permissions;
    if (dangerous_mode) {
        permissions.set_dangerous(true);
        permissions.set_mode(PermissionMode::Yolo);
    }

    // Register built-in safety rules (deny writes to sensitive files/dirs)
    permissions.add_rule({"file_write", "*.env", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_edit", "*.env", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_write", ".git/**", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_edit", ".git/**", "", RuleAction::Deny, 100});
    permissions.add_rule({"bash", "", "rm -rf /", RuleAction::Deny, 100});

    AgentLoop agent_loop(*provider, tools, callbacks, working_dir, permissions);

    // Now that agent_loop exists, update on_busy_changed to drain pending queue
    callbacks.on_busy_changed = [&](bool busy) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.is_waiting = busy;
        if (!busy && !state.pending_queue.empty()) {
            std::string next_prompt = state.pending_queue.front();
            state.pending_queue.erase(state.pending_queue.begin());
            state.conversation.push_back({"user", next_prompt, false});
            state.chat_follow_tail = true;
            clamp_chat_focus();
            state.is_waiting = true;
            std::thread([&agent_loop, next_prompt]() {
                agent_loop.submit(next_prompt);
            }).detach();
        }
        screen.PostEvent(Event::Custom);
    };
    agent_loop.set_callbacks(callbacks);

    // ---- Animation ticker thread ----
    std::atomic<bool> running{true};
    std::thread anim_thread([&] {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            anim_tick++;
            if (state.is_waiting) {
                screen.PostEvent(Event::Custom);
            }
        }
    });

    // ---- Input handling ----
    // Custom input component using paragraph-like flexbox for auto-wrapping.
    // Uses FTXUI's focusCursorBlock so the terminal cursor tracks the caret,
    // which lets the terminal emulator position the IME composition window.
    // NOTE: Renderer must take (bool) to be Focusable. This ensures
    // component_active=true on the element, so the input's cursor always
    // wins focus priority over message_view's | focus (which has
    // component_active=false and cursor_shape=Hidden).
    auto input_renderer = Renderer([&](bool) {
        std::string display_text = state.input_text;
        if (display_text.empty()) {
            return hbox({
                text(" ") | focusCursorBar,
                text("Type your prompt here...") | dim | color(Color::GrayDark),
            });
        }
        // Build paragraph-like flexbox with FTXUI cursor at end
        Elements words;
        std::istringstream ss(display_text);
        std::string word;
        while (std::getline(ss, word, ' ')) {
            words.push_back(text(word));
        }
        if (!words.empty()) {
            words.back() = hbox({words.back(), text(" ") | focusCursorBlock});
        } else {
            words.push_back(text(" ") | focusCursorBlock);
        }
        static const auto config = FlexboxConfig().SetGap(1, 0);
        return flexbox(std::move(words), config);
    });

    // Wrap with CatchEvent to handle all keyboard input
    auto input_with_esc = CatchEvent(input_renderer, [&](Event event) {
        if (event == Event::CtrlC) {
            constexpr auto kCtrlCExitWindow = std::chrono::milliseconds(1200);

            bool should_exit = false;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                auto now = std::chrono::steady_clock::now();
                if (state.ctrl_c_armed && (now - state.last_ctrl_c_time) <= kCtrlCExitWindow) {
                    should_exit = true;
                } else {
                    state.ctrl_c_armed = true;
                    state.last_ctrl_c_time = now;
                    state.conversation.push_back({"system", "Press Ctrl+C again within 1.2s to exit.", false});
                    state.chat_follow_tail = true;
                    clamp_chat_focus();
                }
            }

            if (should_exit) {
                screen.Exit();
            } else {
                screen.PostEvent(Event::Custom);
            }
            return true;
        }

        // Enter → submit message
        if (event == Event::Return) {
            std::lock_guard<std::mutex> lk(state.mu);

            // Handle tool confirmation: y=allow, a=always allow, n/other=deny
            if (state.confirm_pending) {
                std::string answer = state.input_text;
                state.input_text.clear();
                if (!answer.empty() && (answer[0] == 'y' || answer[0] == 'Y')) {
                    state.confirm_result = PermissionResult::Allow;
                } else if (!answer.empty() && (answer[0] == 'a' || answer[0] == 'A')) {
                    state.confirm_result = PermissionResult::AlwaysAllow;
                } else {
                    state.confirm_result = PermissionResult::Deny;
                }
                state.confirm_pending = false;
                state.confirm_cv.notify_one();
                return true;
            }

            if (state.input_text.empty()) return true;
            if (!auth_done) return true;

            std::string prompt = state.input_text;
            state.input_text.clear();

            // Record history
            state.input_history.push_back(prompt);
            state.history_index = -1;

            if (state.is_waiting) {
                state.pending_queue.push_back(prompt);
                state.conversation.push_back({"user", prompt, false});
                state.chat_follow_tail = true;
                clamp_chat_focus();
            } else {
                state.conversation.push_back({"user", prompt, false});
                state.chat_follow_tail = true;
                clamp_chat_focus();
                state.is_waiting = true;
                std::thread([&agent_loop, prompt]() {
                    agent_loop.submit(prompt);
                }).detach();
            }
            return true;
        }
        if (event == Event::PageUp) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (scroll_chat(-5)) {
                screen.PostEvent(Event::Custom);
            }
            return true;
        }
        if (event == Event::PageDown) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (scroll_chat(5)) {
                screen.PostEvent(Event::Custom);
            }
            return true;
        }
        if (event == Event::Home) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (!state.conversation.empty()) {
                state.chat_focus_index = 0;
                state.chat_follow_tail = false;
                screen.PostEvent(Event::Custom);
            }
            return true;
        }
        if (event == Event::End) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (!state.conversation.empty()) {
                state.chat_follow_tail = true;
                clamp_chat_focus();
                screen.PostEvent(Event::Custom);
            }
            return true;
        }
        if (event == Event::Escape) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.confirm_pending) {
                // Escape during confirm → deny
                state.input_text.clear();
                state.confirm_result = PermissionResult::Deny;
                state.confirm_pending = false;
                state.confirm_cv.notify_one();
                return true;
            }
            if (state.is_waiting) {
                agent_loop.cancel();
                return true;
            }
        }
        // Ctrl+P: cycle permission mode
        if (event == Event::Special(std::string(1, '\x10'))) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (!state.is_waiting && !state.confirm_pending) {
                auto new_mode = permissions.cycle_mode();
                state.conversation.push_back({"system",
                    std::string("Permission mode: ") + PermissionManager::mode_name(new_mode) +
                    " - " + PermissionManager::mode_description(new_mode), false});
                clamp_chat_focus();
                screen.PostEvent(Event::Custom);
            }
            return true;
        }
        if (event.is_mouse()) {
            std::lock_guard<std::mutex> lk(state.mu);
            auto& mouse = event.mouse();
            if (!chat_box.Contain(mouse.x, mouse.y)) {
                return false;
            }

            if (mouse.button == Mouse::WheelUp) {
                if (scroll_chat(-3)) {
                    screen.PostEvent(Event::Custom);
                }
                return true;
            }
            if (mouse.button == Mouse::WheelDown) {
                if (scroll_chat(3)) {
                    screen.PostEvent(Event::Custom);
                }
                return true;
            }
        }
        if (event == Event::ArrowUp) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.input_history.empty()) return true;
            if (state.history_index == -1) {
                state.saved_input = state.input_text;
                state.history_index = (int)state.input_history.size() - 1;
            } else if (state.history_index > 0) {
                state.history_index--;
            }
            state.input_text = state.input_history[state.history_index];
            return true;
        }
        if (event == Event::ArrowDown) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.history_index == -1) return true;
            if (state.history_index < (int)state.input_history.size() - 1) {
                state.history_index++;
                state.input_text = state.input_history[state.history_index];
            } else {
                state.history_index = -1;
                state.input_text = state.saved_input;
            }
            return true;
        }
        // Backspace: remove last UTF-8 character
        if (event == Event::Backspace) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (!state.input_text.empty()) {
                size_t pos = state.input_text.size() - 1;
                // Walk back over UTF-8 continuation bytes (10xxxxxx)
                while (pos > 0 && (static_cast<unsigned char>(state.input_text[pos]) & 0xC0) == 0x80) {
                    pos--;
                }
                state.input_text.erase(pos);
            }
            return true;
        }
        // Printable character input
        if (event.is_character()) {
            std::lock_guard<std::mutex> lk(state.mu);
            state.input_text += event.character();
            // Reset history browsing on new input
            state.history_index = -1;
            return true;
        }
        return false;
    });

    auto renderer = Renderer(input_with_esc, [&] {
        std::lock_guard<std::mutex> lk(state.mu);

        // -- Logo --
        auto logo = vbox({
            text("\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80\xE2"),
            text("\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88\xE2\x96\x91\xE2\x96\x88\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80\xE2"),
            text("\xE2\x96\x91\xE2\x96\x80\xE2\x96\x91\xE2\x96\x80\xE2\x96\x91\xE2\x96\x80\xE2\x96\x80\xE2\x96\x80\xE2\x96\x91\xE2\x96\x80\xE2\x96\x80\xE2\x96\x80\xE2"),
        }) | color(Color::Cyan) | bold;

        auto header = hbox({
            text("    "),
            logo,
            filler(),
            vbox({
                text(version_str) | color(Color::GrayLight) | dim,
                text(state.status_line) | color(Color::White),
                text(cwd_display) | color(Color::CyanLight) | dim,
            }),
            text("  "),
        }) | bgcolor(Color::RGB(0, 30, 45));

        // -- Messages --
        Elements message_elements;
        for (size_t i = 0; i < state.conversation.size(); ++i) {
            const auto& msg = state.conversation[i];
            bool focused_message = static_cast<int>(i) == state.chat_focus_index;
            Decorator focus_decorator = focused_message
                ? focusPositionRelative(0.0f, state.chat_follow_tail ? 1.0f : 0.0f)
                : nothing;

            if (msg.role == "user") {
                auto line = hbox({
                    text(" > ") | bold | color(Color::Blue),
                    paragraph(msg.content) | color(Color::White),
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator);
            } else if (msg.role == "assistant") {
                auto line = hbox({
                    text(" * ") | bold | color(Color::Green),
                    paragraph(msg.content) | color(Color::GreenLight),
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator);
            } else if (msg.role == "tool_call") {
                auto line = hbox({
                    text("   -> ") | color(Color::Magenta),
                    paragraph(msg.content) | color(Color::MagentaLight) | dim,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator);
            } else if (msg.role == "tool_result") {
                auto line = hbox({
                    text("   <- ") | color(Color::GrayDark),
                    paragraph(msg.content) | color(Color::GrayLight) | dim,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator);
            } else if (msg.role == "system") {
                auto line = hbox({
                    text(" i ") | bold | color(Color::Yellow),
                    paragraph(msg.content) | color(Color::Yellow),
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator);
            } else if (msg.role == "error") {
                auto line = hbox({
                    text(" ! ") | bold | color(Color::Red),
                    paragraph(msg.content) | color(Color::RedLight),
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator);
            }
            message_elements.push_back(text(""));
        }

        auto message_view = vbox(std::move(message_elements)) | vscroll_indicator | yframe | reflect(chat_box) | flex;

        // -- Thinking indicator --
        Element thinking_element = text("");
        if (state.is_waiting) {
            int tick = anim_tick.load();
            int dot_count = (tick % 3) + 1; // 1, 2, 3 dots cycling
            int wave_pos = tick % 8;         // position of bright wave

            std::string base = "Thinking";
            Elements chars;
            for (int i = 0; i < (int)base.size(); i++) {
                int dist = (i - wave_pos);
                if (dist < 0) dist = -dist;
                Color c;
                if (dist == 0)
                    c = Color::Yellow;
                else if (dist == 1)
                    c = Color::RGB(180, 180, 60);
                else if (dist == 2)
                    c = Color::RGB(120, 120, 40);
                else
                    c = Color::GrayDark;
                chars.push_back(text(std::string(1, base[i])) | color(c));
            }

            // Dots also animate
            for (int i = 0; i < 3; i++) {
                if (i < dot_count)
                    chars.push_back(text(".") | color(Color::Yellow));
                else
                    chars.push_back(text(".") | color(Color::GrayDark));
            }

            thinking_element = hbox({
                text(" \xE2\x97\x8F ") | color(Color::Yellow),
                hbox(std::move(chars)),
            });
        }

        // -- Prompt line --
        Element prompt_line;
        if (state.confirm_pending) {
            prompt_line = hbox({
                text(" [" + state.confirm_tool_name + "] ") | bold | color(Color::Magenta),
                text("y") | bold | color(Color::Green),
                text("es / ") | color(Color::MagentaLight),
                text("a") | bold | color(Color::Cyan),
                text("lways / ") | color(Color::MagentaLight),
                text("n") | bold | color(Color::Red),
                text("o: ") | color(Color::MagentaLight),
                input_with_esc->Render(),
            });
        } else {
            Elements prompt_parts;
            prompt_parts.push_back(text(" > ") | bold | color(Color::Cyan));
            prompt_parts.push_back(input_with_esc->Render());
            if (!state.pending_queue.empty()) {
                prompt_parts.push_back(
                    text(" [" + std::to_string(state.pending_queue.size()) + " queued]") | dim | color(Color::GrayDark));
            }
            prompt_line = hbox(std::move(prompt_parts));
        }

        // -- Bottom status bar --
        std::string perm_mode_str = std::string("mode: ") + PermissionManager::mode_name(permissions.mode());
        Element bottom_bar;
        if (dangerous_mode) {
            bottom_bar = hbox({
                text("  [DANGEROUS MODE]") | bold | color(Color::Red),
                filler(),
                text(perm_mode_str + "  ") | dim | color(Color::GrayDark),
            });
        } else if (state.is_waiting) {
            bottom_bar = hbox({
                text("  esc to interrupt") | dim | color(Color::GrayDark),
                filler(),
                text(perm_mode_str + "  ") | dim | color(Color::GrayDark),
            });
        } else {
            bottom_bar = hbox({
                text("  ctrl+p: cycle permission mode") | dim | color(Color::GrayDark),
                filler(),
                text(perm_mode_str + "  ") | dim | color(Color::GrayDark),
            });
        }

        // IME composition window positioning is handled by FTXUI's cursor
        // system (focusCursorBlock) which emits ANSI sequences to place the
        // terminal cursor at the caret. Windows Terminal/ConPTY uses this
        // to position the IME window. The Win32 IME APIs (ImmSetComposition
        // Window) don't work under ConPTY.

        return vbox({
            header,
            separatorHeavy() | color(Color::GrayDark),
            message_view,
            thinking_element,
            separatorLight() | color(Color::GrayDark),
            prompt_line,
            bottom_bar,
        }) | borderRounded | color(Color::GrayLight);
    });

    screen.Loop(renderer);

    running = false;
    if (anim_thread.joinable()) {
        anim_thread.join();
    }

    if (auth_thread.joinable()) {
        auth_thread.join();
    }

    return 0;
}
