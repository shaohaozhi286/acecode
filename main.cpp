#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>
#include <random>
#include <filesystem>

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
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "version.hpp"
#include "config/config.hpp"
#include "network/proxy_resolver.hpp"
#include "provider/provider_factory.hpp"
#include "provider/copilot_provider.hpp"
#include "provider/model_context_resolver.hpp"
#include "provider/models_dev_registry.hpp"
#include "provider/model_resolver.hpp"
#include "provider/cwd_model_override.hpp"
#include "provider/provider_swap.hpp"
#include "tool/tool_executor.hpp"
#include "tool/bash_tool.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/file_edit_tool.hpp"
#include "tool/grep_tool.hpp"
#include "tool/glob_tool.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/skills_tool.hpp"
#include "tool/skill_view_tool.hpp"
#include "tool/memory_read_tool.hpp"
#include "tool/memory_write_tool.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tool/ask_overlay_input.hpp"
#include "tui/confirm_question.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_commands.hpp"
#include "skills/default_skill_seeder.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "tool/web_search/runtime.hpp"
#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "tool/web_search/web_search_tool.hpp"
#include "utils/logger.hpp"
#include "permissions.hpp"
#include "agent_loop.hpp"
#include "commands/configure.hpp"
#include "daemon/cli.hpp"
#ifdef _WIN32
#  include "daemon/service_win.hpp"
#endif
#include "commands/command_registry.hpp"
#include "commands/builtin_commands.hpp"
#include "commands/compact.hpp"
#include "commands/micro_compact.hpp"
#include "utils/token_tracker.hpp"
#include "markdown/markdown_formatter.hpp"
#include "session/session_manager.hpp"
#include "session/session_resume_restore.hpp"
#include "tui/diff_view.hpp"
#include "tui/paste_handler.hpp"
#include "tui/picker_scroll.hpp"
#include "tui/render_mode_factory.hpp"
#include "utils/terminal_capability.hpp"
#include "utils/state_file.hpp"
#include "tui/slash_dropdown.hpp"
#include "tui/text_truncation.hpp"
#include "tui/thick_vscroll_bar.hpp"
#include "tui/tool_progress.hpp"
#include "utils/base64.hpp"
#include "utils/terminal_title.hpp"
#include "session/session_storage.hpp"
#include "history/input_history_store.hpp"

#include <cstdio>

using namespace ftxui;
using namespace acecode;

namespace {

static const std::string EN_THINKING_PHRASES[50] = {
    "Analyzing", "Pondering", "Investigating", "Synthesizing", "Reviewing",
    "Processing", "Compiling", "Evaluating", "Formulating", "Brainstorming",
    "Searching", "Deciphering", "Gathering", "Debugging", "Inspecting",
    "Generating", "Organizing", "Mapping", "Exploring", "Tracing",
    "Validating", "Considering", "Reflecting", "Simulating", "Calculating",
    "Abstracting", "Diving", "Looking", "Troubleshooting", "Crafting",
    "Polishing", "Assembling", "Connecting", "Building", "Parsing",
    "Extracting", "Tuning", "Optimizing", "Designing", "Theorizing",
    "Hypothesizing", "Seeking", "Interpreting", "Measuring", "Weighing",
    "Reading", "Preparing", "Reasoning", "Constructing", "Finalizing"
};

static const std::string ZH_THINKING_PHRASES[50] = {
    "分析中", "思考中", "研究中", "探索中", "综合中",
    "审查中", "处理中", "编译中", "评估中", "规划中",
    "构思中", "搜索中", "解码中", "收集中", "调试中",
    "检查中", "生成中", "组织中", "映射中", "推理中",
    "验证中", "考虑中", "反思中", "模拟中", "计算中",
    "抽象中", "深挖中", "寻找中", "排查中", "打磨中",
    "完善中", "组装中", "连接中", "构建中", "解析中",
    "提取中", "微调中", "优化中", "设计中", "推论中",
    "假设中", "路线中", "解读中", "测量中", "权衡中",
    "阅读中", "准备中", "追溯中", "构造中", "总结中"
};

static bool is_user_chinese(const acecode::TuiState& state) {
    if (state.conversation.empty()) return false;
    for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
        if (it->role == "user") {
            for (unsigned char c : it->content) {
                if (c >= 0xE0) return true;
            }
            return false;
        }
    }
    return false;
}

static std::string get_random_thinking_phrase(bool is_zh) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 49);
    return is_zh ? ZH_THINKING_PHRASES[dis(gen)] : EN_THINKING_PHRASES[dis(gen)];
}

// A ToolSummary whose metrics contain `exit`, `aborted`, or `timeout` indicates
// a failure; used by the tool_result renderer to pick colour and decide whether
// to show the inline error tail.
static bool is_success_summary(const acecode::ToolSummary& s) {
    for (const auto& kv : s.metrics) {
        if (kv.first == "exit" && kv.second != "0") return false;
        if (kv.first == "aborted" && kv.second == "true") return false;
        if (kv.first == "timeout" && kv.second == "true") return false;
    }
    return true;
}

static std::string renderable_tool_summary_line(const acecode::ToolSummary& s,
                                                const std::string& metric_str,
                                                int max_visual_width) {
    const std::string prefix = s.icon + " " + s.verb + " \xC2\xB7 ";
    const std::string suffix = metric_str.empty()
        ? std::string()
        : " \xC2\xB7 " + metric_str;
    return acecode::tui::truncate_middle_segment(
        prefix, s.object, suffix, max_visual_width);
}

static Element render_tool_result_lines_preserving_breaks(
    const std::string& display_content) {
    Elements lines;
    size_t pos = 0;
    while (pos <= display_content.size()) {
        const size_t nl = display_content.find('\n', pos);
        const std::string line = (nl == std::string::npos)
            ? display_content.substr(pos)
            : display_content.substr(pos, nl - pos);
        Element line_el = line.empty() ? text(" ") : paragraph(line);
        lines.push_back(line_el | color(Color::GrayLight) | dim);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return vbox(std::move(lines));
}


bool is_space_glyph(const std::string& glyph) {
    return glyph == " " || glyph == "\t";
}

bool is_narrow_glyph(const std::string& glyph) {
    return ftxui::string_width(glyph) == 1;
}

bool is_opening_cjk_punctuation(const std::string& glyph) {
    static constexpr std::array<std::string_view, 8> kOpening = {
        "（", "《", "「", "【", "‘", "“", "〈", "『"
    };
    for (const auto& candidate : kOpening) {
        if (glyph == candidate) {
            return true;
        }
    }
    return false;
}

bool is_closing_cjk_punctuation(const std::string& glyph) {
    static constexpr std::array<std::string_view, 15> kClosing = {
        "，", "。", "！", "？", "；", "：", "、", "）",
        "》", "」", "】", "’", "”", "〉", "』"
    };
    for (const auto& candidate : kClosing) {
        if (glyph == candidate) {
            return true;
        }
    }
    return false;
}

void flush_ascii_run(std::string* ascii_run,
                     std::string* pending_prefix,
                     std::vector<std::string>* output) {
    if (ascii_run->empty()) {
        return;
    }

    std::string token = std::move(*ascii_run);
    ascii_run->clear();
    if (!pending_prefix->empty()) {
        token = std::move(*pending_prefix) + token;
        pending_prefix->clear();
    }
    output->push_back(std::move(token));
}

std::vector<std::string> tokenize_wrapped_input(const std::string& text) {
    std::vector<std::string> tokens;
    std::string ascii_run;
    std::string pending_prefix;

    for (const auto& glyph : ftxui::Utf8ToGlyphs(text)) {
        if (glyph.empty()) {
            continue;
        }

        if (is_space_glyph(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            if (!tokens.empty()) {
                tokens.back() += " ";
            }
            continue;
        }

        if (is_opening_cjk_punctuation(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            pending_prefix += glyph;
            continue;
        }

        if (is_closing_cjk_punctuation(glyph)) {
            flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
            if (!tokens.empty()) {
                tokens.back() += glyph;
            } else if (!pending_prefix.empty()) {
                pending_prefix += glyph;
            } else {
                tokens.push_back(glyph);
            }
            continue;
        }

        if (is_narrow_glyph(glyph)) {
            ascii_run += glyph;
            continue;
        }

        flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
        std::string token = glyph;
        if (!pending_prefix.empty()) {
            token = std::move(pending_prefix) + token;
            pending_prefix.clear();
        }
        tokens.push_back(std::move(token));
    }

    flush_ascii_run(&ascii_run, &pending_prefix, &tokens);
    if (!pending_prefix.empty()) {
        if (!tokens.empty()) {
            tokens.back() += pending_prefix;
        } else {
            tokens.push_back(std::move(pending_prefix));
        }
    }

    return tokens;
}

Element render_wrapped_input_text(const std::string& input_value, size_t cursor_bytes) {
    if (cursor_bytes > input_value.size()) cursor_bytes = input_value.size();

    // Split input into head/cursor_glyph/tail so the caret block can be drawn
    // over the glyph under the caret (or a space when the caret sits at end).
    std::string head = input_value.substr(0, cursor_bytes);
    std::string cursor_glyph;
    std::string tail;
    if (cursor_bytes < input_value.size()) {
        size_t next = cursor_bytes + 1;
        while (next < input_value.size() &&
               (static_cast<unsigned char>(input_value[next]) & 0xC0) == 0x80) {
            next++;
        }
        cursor_glyph = input_value.substr(cursor_bytes, next - cursor_bytes);
        tail = input_value.substr(next);
    }

    auto tokens_head = tokenize_wrapped_input(head);
    auto tokens_tail = tokenize_wrapped_input(tail);

    auto cursor_elem = ftxui::text(cursor_glyph.empty() ? std::string(" ") : cursor_glyph)
                       | focusCursorBlock;

    if (tokens_head.empty() && tokens_tail.empty()) {
        return cursor_elem;
    }

    Elements parts;
    parts.reserve(tokens_head.size() + tokens_tail.size() + 1);

    // Emit all but the last head token as standalone flex items.
    for (size_t i = 0; i + 1 < tokens_head.size(); ++i) {
        parts.push_back(ftxui::text(std::move(tokens_head[i])));
    }

    // Fuse (last_head_token, cursor_elem, first_tail_token) into one hbox so
    // the caret never lands at a natural wrap boundary.
    Elements compound;
    if (!tokens_head.empty()) {
        compound.push_back(ftxui::text(std::move(tokens_head.back())));
    }
    compound.push_back(cursor_elem);
    size_t tail_start = 0;
    if (!tokens_tail.empty()) {
        compound.push_back(ftxui::text(std::move(tokens_tail[0])));
        tail_start = 1;
    }
    parts.push_back(hbox(std::move(compound)));

    for (size_t i = tail_start; i < tokens_tail.size(); ++i) {
        parts.push_back(ftxui::text(std::move(tokens_tail[i])));
    }

    static const auto config = FlexboxConfig().SetGap(0, 0);
    return flexbox(std::move(parts), config);
}

}  // namespace

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

static std::string get_executable_dir_from_argv(int argc, char* argv[]) {
    if (argc <= 0 || !argv[0]) return "";
    std::error_code ec;
    std::filesystem::path exe(argv[0]);
    std::filesystem::path abs = std::filesystem::weakly_canonical(exe, ec);
    if (!ec) return abs.parent_path().string();
    return exe.parent_path().string();
}

static void seed_default_skills_if_first_initialization(const std::string& argv0_dir) {
    bool first_initialization = acecode::consume_acecode_home_created_by_process();
    auto result = acecode::install_default_global_skills_on_first_initialization(
        std::filesystem::path(acecode::get_acecode_dir()),
        argv0_dir,
        first_initialization);
    if (!result.attempted) return;

    size_t installed = 0;
    size_t skipped = 0;
    size_t errors = 0;
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

// ---- Reset terminal cursor visibility on exit ----
static void reset_cursor() {
    // DECTCEM: show cursor (ESC [ ? 25 h)
    std::cout << "\033[?25h" << std::flush;
}

// ---- Session finalization on exit ----
static SessionManager* g_session_manager = nullptr;

// Active FTXUI screen, used by signal / console-ctrl handlers to trigger a
// graceful Loop exit. Must be cleared before ScreenInteractive is destroyed
// so handlers can't dereference a dead object.
// App::Exit() is thread-safe (posts a task internally — see app.cpp:1063).
static std::atomic<ftxui::ScreenInteractive*> g_active_screen{nullptr};

static void finalize_session_atexit() {
    if (g_session_manager) {
        g_session_manager->finalize();
        auto sid = g_session_manager->current_session_id();
        if (!sid.empty()) {
            std::cerr << "\nacecode: session " << sid
                      << " saved. Resume with: acecode --resume " << sid << std::endl;
        }
    }
    // Best-effort: hand the window title back to the parent shell. Not
    // strictly async-signal-safe but consistent with the existing finalize
    // path which already uses iostreams.
    clear_terminal_title();
}

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    // Normal Ctrl+C is handled through stdin once ENABLE_PROCESSED_INPUT is
    // cleared after FTXUI installs its terminal mode. This handler remains as
    // a fallback for hosts that still deliver CTRL_C_EVENT, and for
    // Ctrl+Break/close events where graceful shutdown is more important than
    // prompting.
    auto* s = g_active_screen.load(std::memory_order_acquire);
    if (ctrl_type == CTRL_C_EVENT) {
        if (s) {
            s->PostEvent(ftxui::Event::CtrlC);
            return TRUE;
        }
        finalize_session_atexit();
        return FALSE;
    }
    if (ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
        // Break / 关窗:明确退出意图,直接走 FTXUI 优雅退出 —— Loop 返回后
        // ScreenInteractive 析构跑 on_exit_functions,把 alt-screen /
        // mouse tracking / Windows console mode 还原回去。返回 FALSE 会让
        // 默认 handler TerminateProcess(),终端会留在 mouse-tracking 开
        // 的状态,父 shell 收到鼠标事件原样喷成乱码字节。
        if (s) {
            s->Exit();
            return TRUE;
        }
        finalize_session_atexit();
        return FALSE;
    }
    return FALSE;
}

static void prepare_windows_ctrl_c_handling_after_ftxui_install() {
    auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = 0;
    if (stdin_handle != INVALID_HANDLE_VALUE &&
        GetConsoleMode(stdin_handle, &in_mode)) {
        // FTXUI preserves ENABLE_PROCESSED_INPUT from the original console
        // mode. When it stays enabled, Windows turns Ctrl+C into a console
        // control event instead of a stdin byte, and FTXUI's SIGINT handler can
        // exit before our Event::CtrlC branch gets a chance to show the prompt.
        SetConsoleMode(stdin_handle, in_mode & ~ENABLE_PROCESSED_INPUT);
    }

    // Re-register after FTXUI's Loop/PreMain installs its own signal handling
    // so this handler gets first chance at CTRL_BREAK_EVENT / CTRL_CLOSE_EVENT
    // and any CTRL_C_EVENT fallback.
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}
#else
#include <csignal>
static void signal_handler(int /*sig*/) {
    // FTXUI overrides SIGINT/SIGTERM during Loop() (app.cpp:575) so this only
    // fires before Loop starts or after it returns — terminal isn't in the
    // raw/alt-screen state yet, so _exit is safe.
    finalize_session_atexit();
    _exit(1);
}
#endif

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
// TuiState is defined in src/tui_state.hpp
#include "tui_state.hpp"
using acecode::TuiState;

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // SECURITY: Prevent Windows from executing commands from current directory.
    // Without this, a malicious exe placed in cwd could hijack system commands.
    SetEnvironmentVariableA("NoDefaultCurrentDirectoryInExePath", "1");
#endif

    // ---- Top-level subcommand dispatch ----
    // `acecode daemon ...` 完全脱离 TUI,在第一时间分流出去。所有其它路径继续走
    // 下面的 TUI argv 解析。
    if (argc >= 2 && std::string(argv[1]) == "daemon") {
        std::vector<std::string> tokens;
        for (int i = 2; i < argc; ++i) tokens.emplace_back(argv[i]);
        std::string exe_path = (argc > 0 && argv[0]) ? std::string(argv[0]) : "";
        return acecode::daemon::cli::run(tokens, exe_path);
    }

    // `acecode service ...` — Windows Service 包装(spec design.md Decision 8)。
    // Linux/macOS 暂不支持原生 service install,提示走 systemd / launchd。
    if (argc >= 2 && std::string(argv[1]) == "service") {
#ifdef _WIN32
        std::vector<std::string> tokens;
        for (int i = 2; i < argc; ++i) tokens.emplace_back(argv[i]);
        std::string exe_path = (argc > 0 && argv[0]) ? std::string(argv[0]) : "";
        return acecode::daemon::service_win::run_cli(tokens, exe_path);
#else
        std::cerr << "acecode: native `service` subcommand is Windows-only;\n"
                     "         on Linux/macOS use `acecode daemon --foreground`\n"
                     "         under systemd / launchd (see README for sample units).\n";
        return 65;
#endif
    }

    // ServiceMain detection — SCM 启动时 argv 含 --service-main 标记;转入
    // StartServiceCtrlDispatcher 阻塞等 SCM 调度。Windows-only;非 Windows 给清晰错误。
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--service-main") {
#ifdef _WIN32
            return acecode::daemon::service_win::run_service_main_dispatcher();
#else
            std::cerr << "--service-main is Windows-only\n";
            return 64;
#endif
        }
    }

    std::string argv0_dir = get_executable_dir_from_argv(argc, argv);

    // 5.7: 双击启动检测(Windows)。无参数 + 控制台只挂着自己一个进程
    // (GetConsoleProcessList 返回 1)= 双击;此时按 daemon.auto_start_on_double_click
    // 决定走 TUI 还是 daemon detach。
#ifdef _WIN32
    if (argc == 1) {
        AppConfig cfg_probe = load_config();
        if (cfg_probe.daemon.auto_start_on_double_click) {
            DWORD procs[2] = {0, 0};
            DWORD n = ::GetConsoleProcessList(procs, 2);
            if (n == 1) {
                std::vector<std::string> tokens = {"start"};
                std::string exe_path = argv[0] ? std::string(argv[0]) : "";
                return acecode::daemon::cli::run(tokens, exe_path);
            }
        }
    }
#endif

    // ---- Parse CLI arguments ----
    bool dangerous_mode = false;
    bool run_configure_cmd = false;
    bool validate_models_registry_cmd = false;
    bool resume_latest = false;
    bool force_alt_screen = false;  // --alt-screen / -alt-screen 强制 fallback
    std::string resume_session_id;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-dangerous" || arg == "--dangerous") {
            dangerous_mode = true;
        } else if (arg == "configure") {
            run_configure_cmd = true;
        } else if (arg == "--validate-models-registry") {
            validate_models_registry_cmd = true;
        } else if (arg == "-alt-screen" || arg == "--alt-screen") {
            force_alt_screen = true;
        } else if (arg == "--resume") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                resume_session_id = argv[++i];
            } else {
                resume_latest = true;
            }
        }
    }

    // ---- Handle configure subcommand (before TUI setup) ----
    if (run_configure_cmd) {
        AppConfig config = load_config();
        seed_default_skills_if_first_initialization(argv0_dir);
        initialize_registry(config, argv0_dir);
        return run_configure(config);
    }

    // ---- Handle --validate-models-registry (CI helper) ----
    if (validate_models_registry_cmd) {
        AppConfig config = load_config();
        seed_default_skills_if_first_initialization(argv0_dir);
        initialize_registry(config, argv0_dir);
        const auto& src = current_registry_source();
        auto registry = current_registry();
        if (!registry || registry->empty()) {
            std::cerr << "models.dev registry not found or empty\n";
            return 1;
        }
        size_t actual_models = 0;
        for (auto it = registry->begin(); it != registry->end(); ++it) {
            if (!it->is_object()) continue;
            auto m = it->find("models");
            if (m == it->end()) continue;
            if (m->is_object()) actual_models += m->size();
            else if (m->is_array()) actual_models += m->size();
        }
        std::cout << "models.dev registry OK: " << registry->size() << " providers, "
                  << actual_models << " models, source=" << src.path_or_url << "\n";
        if (src.manifest && src.manifest->is_object()) {
            const auto& m = *src.manifest;
            if (m.contains("model_count") && m["model_count"].is_number_integer()) {
                size_t expected = static_cast<size_t>(m["model_count"].get<int>());
                if (expected != actual_models) {
                    std::cerr << "MANIFEST.json model_count=" << expected
                              << " disagrees with actual " << actual_models << "\n";
                    return 1;
                }
            }
        }
        return 0;
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
    seed_default_skills_if_first_initialization(argv0_dir);

    // ---- Init proxy resolver ----
    // 必须在任何 cpr 调用之前完成(下面 initialize_registry 可能触发 models.dev
    // 拉取)。失败也不应阻塞启动 —— ProxyResolver 内部所有探测都是 soft-fail。
    network::proxy_resolver().init(config.network);
    {
        auto resolved = network::proxy_resolver().effective("https://example.com");
        std::string url_disp = resolved.url.empty()
                                  ? std::string("direct")
                                  : network::redact_credentials(resolved.url);
        std::string banner = "Proxy: " + url_disp + " (" + resolved.source + ")";
        if (config.network.proxy_insecure_skip_verify) {
            // ANSI red bold — TUI 还没起来,直接打 stderr 让用户立刻看见。
            banner += "  \x1b[1;31m[INSECURE: TLS verification disabled]\x1b[0m";
            LOG_WARN("[proxy] insecure_skip_verify is enabled — TLS chain validation off");
        }
        std::cerr << banner << std::endl;
        LOG_INFO(std::string("[proxy] effective=") +
                 (resolved.url.empty() ? "direct" : network::redact_credentials(resolved.url)) +
                 " source=" + resolved.source +
                 " mode=" + config.network.proxy_mode);
    }

    // ---- Load bundled models.dev registry (offline-first) ----
    initialize_registry(config, argv0_dir);
    if (config.models_dev.allow_network && !config.models_dev.refresh_on_command_only) {
        std::thread([] {
            refresh_registry_from_network();
        }).detach();
    }

    // ---- Resolve effective model + create provider ----
    // 三级回退:default → cwd override → session meta(resume 路径在后面单独应用)。
    // 启动时这里只看前两级;resume 的 meta 应用延到下面读到 meta 之后。
    auto cwd_override = load_cwd_model_override(working_dir);
    ModelProfile effective_entry = resolve_effective_model(config, cwd_override, std::nullopt);
    std::shared_ptr<LlmProvider> provider = create_provider_from_entry(effective_entry);
    std::mutex provider_mu;  // 保护 provider 替换操作(design D4)
    auto provider_accessor = [&provider, &provider_mu]() -> std::shared_ptr<LlmProvider> {
        std::lock_guard<std::mutex> lk(provider_mu);
        return provider;
    };
    config.context_window = resolve_model_context_window(
        config,
        provider->name(),
        provider->model(),
        config.context_window
    );

    // ---- Init web search runtime ----
    // Backend router + region detector 单例,异步探测一次后写缓存。失败 / 已有
    // 缓存都不阻塞启动。enabled=false 时仍 init,但下面 register 阶段不挂工具。
    web_search::init(config.web_search);
    web_search::register_default_backends(web_search::runtime().router(),
                                           config.web_search);
    {
        // 启动时先用缓存 region(若有)即时 resolve;无缓存先按 Unknown 走悲观,
        // 再起 detached 线程做实际 HEAD 探测,完成后再 resolve 一次。
        web_search::Region cached = web_search::runtime().detector().cached_region();
        web_search::runtime().router().resolve_active(cached);
        if (cached == web_search::Region::Unknown) {
            std::thread([]{
                auto r = web_search::runtime().detector().detect_now();
                web_search::runtime().router().resolve_active(r);
            }).detach();
        }
    }

    // ---- Setup tools ----
    ToolExecutor tools;
    tools.register_tool(create_bash_tool());
    tools.register_tool(create_file_read_tool());
    tools.register_tool(create_file_write_tool());
    tools.register_tool(create_file_edit_tool());
    tools.register_tool(create_grep_tool());
    tools.register_tool(create_glob_tool());
    tools.register_tool(create_task_complete_tool());
    if (config.web_search.enabled) {
        tools.register_tool(web_search::create_web_search_tool(
            web_search::runtime().router(), web_search::runtime().cfg()));
    }

    // ---- Skill registry ----
    // Discovery order keeps more specific roots ahead of compatibility/global
    // roots because SkillRegistry is first-wins by skill name:
    //   1) project walk  — <cwd...>/.acecode/skills, deepest first, up to (not including) HOME
    //   2) project walk  — <cwd...>/.agent/skills, deepest first, up to (not including) HOME
    //   3) user global   — ~/.acecode/skills (auto-created)
    //   4) user global   — ~/.agent/skills (compatibility root; left as-is if missing)
    //   5) external dirs — config.skills.external_dirs (absolute or ~-expanded)
    SkillRegistry skill_registry;
    {
        std::vector<std::filesystem::path> roots;
        std::error_code ec;

        for (const auto& dir : get_project_dirs_up_to_home(get_cwd())) {
            roots.emplace_back(std::filesystem::path(dir) / ".acecode" / "skills");
        }
        for (const auto& dir : get_project_dirs_up_to_home(get_cwd())) {
            roots.emplace_back(std::filesystem::path(dir) / ".agent" / "skills");
        }

        std::string default_acecode_skills_dir =
            (std::filesystem::path(get_acecode_dir()) / "skills").string();
        if (!std::filesystem::exists(default_acecode_skills_dir, ec)) {
            std::filesystem::create_directories(default_acecode_skills_dir, ec);
        }
        roots.emplace_back(default_acecode_skills_dir);

        std::string default_agent_skills_dir = expand_path("~/.agent/skills");
        roots.emplace_back(default_agent_skills_dir);

        for (const auto& raw : config.skills.external_dirs) {
            std::string expanded = expand_path(raw);
            if (!expanded.empty()) roots.emplace_back(expanded);
        }

        skill_registry.set_scan_roots(std::move(roots));
        skill_registry.set_disabled(std::unordered_set<std::string>(
            config.skills.disabled.begin(), config.skills.disabled.end()));
        skill_registry.scan();
    }
    tools.register_tool(create_skills_list_tool(skill_registry));
    tools.register_tool(create_skill_view_tool(skill_registry));

    // ---- Memory registry ----
    // Auto-create ~/.acecode/memory/ if missing; failure disables the memory
    // system for this session without rewriting the user's config.json.
    MemoryRegistry memory_registry;
    MemoryConfig runtime_memory_cfg = config.memory;
    {
        std::error_code mkec;
        std::filesystem::create_directories(get_memory_dir(), mkec);
        if (mkec) {
            LOG_ERROR("[memory] failed to create " + get_memory_dir().generic_string() +
                      ": " + mkec.message() + " — memory will be disabled this session");
            runtime_memory_cfg.enabled = false;
        } else if (runtime_memory_cfg.enabled) {
            memory_registry.scan();
        }
    }
    tools.register_tool(create_memory_read_tool(memory_registry,
                                                runtime_memory_cfg.max_index_bytes));
    tools.register_tool(create_memory_write_tool(memory_registry));

    // ---- MCP servers ----
    McpManager mcp_manager;
    {
        const size_t configured = config.mcp_servers.size();
        if (configured > 0) {
            mcp_manager.connect_all(config);
            mcp_manager.register_tools(tools);
            const size_t connected = mcp_manager.connected_server_count();
            const size_t tool_count = mcp_manager.discovered_tool_count();
            LOG_INFO("[mcp] Connected " + std::to_string(connected) + "/" +
                     std::to_string(configured) + " servers, registered " +
                     std::to_string(tool_count) + " tools");
        }
    }

    // ---- TUI state ----
    TuiState state;
    state.status_line = "[" + provider->name() + "] model: " +
        (config.provider == "copilot" ? config.copilot.model : config.openai.model);

    // Restore per-working-directory input history. Independent of session files so
    // /clear, resume, or deleting a session never blows away the ↑/↓ queue.
    // 关闭开关时退化为纯内存历史（旧行为）。
    if (config.input_history.enabled) {
        std::string ih_path = InputHistoryStore::file_path(
            SessionStorage::get_project_dir(working_dir));
        state.input_history = InputHistoryStore::load(ih_path);
        // 若历史文件条数超过当前上限（用户把 max_entries 调小），保留最近 N 条。
        int cap = config.input_history.max_entries;
        if (cap > 0 && (int)state.input_history.size() > cap) {
            state.input_history.erase(
                state.input_history.begin(),
                state.input_history.begin() + (state.input_history.size() - (size_t)cap));
        }
    }

    // Version and working directory strings for TUI header
    std::string version_str = "acecode v" ACECODE_VERSION;
    std::string cwd_display = working_dir;

    // If dangerous mode, show startup warning. Note: -dangerous skips permission
    // confirmations but does NOT suppress AskUserQuestion overlays (those are a
    // legitimate LLM-driven request for input).
    if (dangerous_mode) {
        state.conversation.push_back({"system",
            "[DANGEROUS MODE] All permission checks are bypassed. Use with caution! "
            "(AskUserQuestion overlays are still shown when the model needs input.)",
            false});
    }

    if (mcp_manager.connected_server_count() > 0) {
        state.conversation.push_back({"system",
            "[MCP] Connected " + std::to_string(mcp_manager.connected_server_count()) +
            " server(s), registered " + std::to_string(mcp_manager.discovered_tool_count()) +
            " external tool(s).", false});
    }

    // Animation tick for Thinking... indicator
    std::atomic<int> anim_tick{0};
    Box chat_box;
    // draggable-thick-scrollbar: track box for the thick scroll indicator —
    // populated by acecode::tui::thick_vscroll_bar each Render so the mouse
    // handler can hit-test "click on scrollbar" vs "click in chat content".
    Box scrollbar_box;

    // drag-autoscroll: 每帧渲染时由 reflect 回填每条消息的屏幕 box,
    // 下一帧 (事件线程 / anim_thread) 读这里算每条消息的行数,供
    // scroll_chat_by_lines 做按行粒度的平滑滚动. Renderer 重新构建 element
    // 树时先把上一帧的高度同步到 message_line_counts, 再把 boxes 清零.
    // 读写发生在 (a) 事件线程 Renderer callback (b) anim_thread 通过
    // state.mu 保护下的 scroll_chat_by_lines, 不显式加锁 — 与现有 state
    // 读取路径保持一致的 "PostEvent happens-before" 模型.
    std::vector<Box> message_boxes;
    std::vector<int> message_line_counts;

    // 终端能力探测 + render mode 决策(add-legacy-terminal-fallback)。
    // CLI --alt-screen / -alt-screen 把 effective alt_screen_mode 临时强制为
    // "always",优先级高于 config.json,只影响本次启动,不写回。
    auto term_caps = acecode::detect_terminal_capabilities();
    if (force_alt_screen) {
        config.tui.alt_screen_mode = "always";
    }
    auto render_mode = acecode::tui::decide_render_mode(config.tui, term_caps);

    // 一次性提示:仅在自动探测命中并切换到 alt-screen 时显示一次。
    // CLI 强制 / config "always" / config "never" / 现代终端都不触发。
    bool show_legacy_hint =
        render_mode == acecode::tui::ScreenRenderMode::AltScreen &&
        !force_alt_screen &&
        config.tui.alt_screen_mode == "auto" &&
        !term_caps.source_label.empty() &&
        !acecode::read_state_flag("legacy_terminal_hint_shown");
    if (show_legacy_hint) {
        std::string hint = "提示: 检测到 " + term_caps.source_label +
            ",已启用全屏渲染避免画面跳动。可在 ~/.acecode/config.json 设置 "
            "\"tui\": {\"alt_screen_mode\": \"never\"} 关闭。";
        state.conversation.push_back({"system", hint, false});
        acecode::write_state_flag("legacy_terminal_hint_shown", true);
    }

    auto screen = acecode::tui::make_screen_interactive(render_mode);
    // Publish for the Windows console-ctrl handler so Ctrl+C can trigger a
    // graceful Loop exit instead of letting the default handler kill us.
    g_active_screen.store(&screen, std::memory_order_release);
    screen.ForceHandleCtrlC(false);
    // mouse-selection-copy: register a no-op SelectionChange callback so
    // FTXUI enables live selection tracking. The right-click branch in the
    // CatchEvent handler reads screen.GetSelection() on demand; we don't
    // need per-change work here yet. Future: hook "auto-clear on new drag".
    screen.SelectionChange([]{});

    // AskUserQuestion 依赖 TuiState + ScreenInteractive 才能发起阻塞 overlay,
    // 所以和其它无依赖的内置工具分开、等 `state` / `screen` 就绪之后再注册。
    tools.register_tool(create_ask_user_question_tool(state, screen));

    auto clamp_chat_focus = [&state]() {
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

    // 按行粒度滚动: 鼠标滚轮 / PgUp / PgDn / 拖到边界 auto-scroll 全部走它.
    // 实现:在 chat_focus_index + chat_line_offset 维护"焦点行"的位置, 穿越
    // 当前消息的行数边界时进位到相邻消息. 返回实际滚了几行, 供调用方调用
    // screen.ShiftSelection(0, -actual) 补偿 FTXUI 屏幕坐标选区.
    // 注意: 本 lambda 必须在持有 state.mu 的上下文中调用.
    auto scroll_chat_by_lines = [&state, &message_line_counts](int delta_lines) -> int {
        if (state.conversation.empty()) return 0;
        int last_msg = static_cast<int>(state.conversation.size()) - 1;
        if (state.chat_focus_index < 0) {
            state.chat_focus_index = last_msg;
        }

        auto lines_of = [&](int idx) -> int {
            if (idx < 0 || idx >= static_cast<int>(message_line_counts.size())) return 1;
            int h = message_line_counts[idx];
            return h > 0 ? h : 1;
        };

        int actual = 0;
        while (delta_lines != 0) {
            int dir = delta_lines > 0 ? 1 : -1;
            int cur = state.chat_focus_index;
            int cur_lines = lines_of(cur);
            int next_offset = state.chat_line_offset + dir;
            if (next_offset < 0) {
                if (cur <= 0) break;  // 已到最顶
                state.chat_focus_index = cur - 1;
                state.chat_line_offset = lines_of(cur - 1) - 1;
            } else if (next_offset >= cur_lines) {
                if (cur >= last_msg) break;  // 已到最底
                state.chat_focus_index = cur + 1;
                state.chat_line_offset = 0;
            } else {
                state.chat_line_offset = next_offset;
            }
            actual += dir;
            delta_lines -= dir;
        }
        // 到达最后一条消息的最末行就重新 follow_tail —— 适用于鼠标滚轮 / PgDn /
        // 拖到底三种场景:用户主动滚到底就期望新内容自动跟进。其他位置保持手动锁定。
        int last_lines = lines_of(last_msg);
        state.chat_follow_tail = (state.chat_focus_index == last_msg &&
                                  state.chat_line_offset >= last_lines - 1);
        return actual;
    };

    // ---- Copilot auth flow (background thread) ----
    std::atomic<bool> auth_done{false};
    std::thread auth_thread;

    if (config.provider == "copilot") {
        auto* copilot = dynamic_cast<CopilotProvider*>(provider.get());
        if (copilot && !copilot->is_authenticated()) {
            {
                std::lock_guard<std::mutex> lk(state.mu);
                state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
                // 新一轮等待：计时/计数字段必须和 is_waiting 一起重置，否则
                // on_busy_changed 的 `busy && !is_waiting` 护栏会把这段跳过，
                // thinking_start_time 会停在 time_point{} 原点，底部秒数会巨大。
                state.thinking_start_time = std::chrono::steady_clock::now();
                state.streaming_output_chars = 0;
                state.last_completion_tokens_authoritative = 0;
                state.is_waiting = true;
                state.conversation.push_back({"system", "Authenticating with GitHub Copilot...", false});
            }
            screen.PostEvent(Event::Custom);

            auth_thread = std::thread([copilot, &state, &screen, &auth_done, &config] {
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

                copilot->run_device_flow([&state, &screen](const std::string& status) {
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

    // ---- Token tracking ----
    TokenTracker token_tracker;
    state.token_status = token_tracker.format_status(config.context_window);

    // ---- Agent callbacks ----
    std::atomic<bool> agent_aborting{false};  // shared abort flag for confirm_cv
    AgentCallbacks callbacks;
    callbacks.on_message = [&state, &clamp_chat_focus, &screen](const std::string& role, const std::string& content, bool is_tool) {
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
    callbacks.on_busy_changed = [&state, &screen](bool busy) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (busy && !state.is_waiting) {
            state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
            state.thinking_start_time = std::chrono::steady_clock::now();
            state.streaming_output_chars = 0;
            state.last_completion_tokens_authoritative = 0;
        }
        state.is_waiting = busy;
        screen.PostEvent(Event::Custom);
    };
    callbacks.on_tool_confirm = [&state, &screen, &agent_aborting](const std::string& tool_name, const std::string& args) -> PermissionResult {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.confirm_pending = true;
            state.confirm_tool_name = tool_name;
            state.confirm_tool_args = args;
            // 每次新确认都把焦点复位到 "No",避免上一次留下的焦点泄漏到下一次,
            // 同时安全的默认是 Deny —— 用户随手 Enter 不会误授权。
            state.confirm_focus = 2;
        }
        screen.PostEvent(Event::Custom);

        // Block the agent thread until the user responds in the TUI (or abort)
        std::unique_lock<std::mutex> lk(state.mu);
        state.confirm_cv.wait(lk, [&state, &agent_aborting] {
            return !state.confirm_pending || agent_aborting.load();
        });
        if (agent_aborting.load()) return PermissionResult::Deny;
        return state.confirm_result;
    };
    callbacks.on_delta = [&state, &clamp_chat_focus, &screen](const std::string& token) {
        std::lock_guard<std::mutex> lk(state.mu);
        // Find or create the streaming assistant message
        if (state.conversation.empty() ||
            state.conversation.back().role != "assistant" ||
            state.conversation.back().is_tool) {
            state.conversation.push_back({"assistant", "", false});
        }
        state.conversation.back().content += token;
        state.streaming_output_chars += token.size();
        clamp_chat_focus();
        screen.PostEvent(Event::Custom);
    };
    // Attach summary/display_override to the two most-recent TUI messages
    // (the trailing tool_call row and tool_result row that on_message just
    // appended) so the renderer can switch to the single-line summary mode.
    callbacks.on_tool_result = [&state, &screen](const ChatMessage& call_msg,
                                                 const std::string& /*tool_name*/,
                                                 const ToolResult& result) {
        std::lock_guard<std::mutex> lk(state.mu);
        // Walk the tail backwards: most recent tool_result gets `summary` +
        // `hunks`, the nearest preceding tool_call gets `display_override`.
        // Both were just pushed by `on_message` on the agent worker thread.
        for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
            if (it->role == "tool_result" && !it->summary.has_value()) {
                it->summary = result.summary;
                it->hunks = result.hunks;
                break;
            }
        }
        if (!call_msg.display_override.empty()) {
            for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
                if (it->role == "tool_call" && it->display_override.empty()) {
                    it->display_override = call_msg.display_override;
                    break;
                }
            }
        }
        screen.PostEvent(Event::Custom);
    };
    callbacks.on_usage = [&token_tracker, &state, &config, &screen](const TokenUsage& usage) {
        token_tracker.record(usage);
        std::lock_guard<std::mutex> lk(state.mu);
        state.token_status = token_tracker.format_status(config.context_window);
        state.last_completion_tokens_authoritative = usage.completion_tokens;
        screen.PostEvent(Event::Custom);
    };
    callbacks.on_auto_compact = [&state, &clamp_chat_focus, &screen]() -> bool {
        std::lock_guard<std::mutex> lk(state.mu);
        state.conversation.push_back({"system", "[Auto-compact] Context approaching limit, compacting...", false});
        clamp_chat_focus();
        screen.PostEvent(Event::Custom);
        return false;
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

    AgentLoop agent_loop(provider_accessor, tools, callbacks, working_dir, permissions);
    agent_loop.set_context_window(config.context_window);
    agent_loop.set_agent_loop_config(config.agent_loop);
    agent_loop.set_skill_registry(&skill_registry);
    agent_loop.set_memory_registry(&memory_registry);
    agent_loop.set_memory_config(&runtime_memory_cfg);
    agent_loop.set_project_instructions_config(&config.project_instructions);

    // Auto-compact tracking state for circuit breaker
    AutoCompactTrackingState compact_tracking;

    callbacks.on_auto_compact = [&state, &clamp_chat_focus, &screen, &agent_loop, &provider,
                                  &compact_tracking, &config, &token_tracker]() -> bool {
        // Circuit breaker: stop after consecutive failures
        if (compact_tracking.consecutive_failures >= MAX_CONSECUTIVE_AUTOCOMPACT_FAILURES) {
            LOG_WARN("Auto-compact circuit breaker tripped (" +
                     std::to_string(compact_tracking.consecutive_failures) + " consecutive failures)");
            return false;
        }

        compact_tracking.turn_counter++;

        // Phase 1: Try micro-compact first
        {
            auto [boundary_start, boundary_count] = get_messages_after_compact_boundary(agent_loop.messages());
            auto& msgs = agent_loop.messages_mut();
            int pre_tokens = estimate_message_tokens(
                std::vector<ChatMessage>(msgs.begin() + boundary_start, msgs.end()));

            auto micro_result = run_micro_compact(msgs, boundary_start);
            if (micro_result.performed) {
                // Insert microcompact boundary marker
                auto mc_boundary = create_microcompact_boundary_message(
                    pre_tokens, micro_result.estimated_tokens_saved, micro_result.cleared_tool_call_ids);
                msgs.push_back(mc_boundary);

                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    std::ostringstream oss;
                    oss << "[Micro-compact] Cleared " << micro_result.tool_results_cleared
                        << " old tool results, saved ~"
                        << TokenTracker::format_tokens(micro_result.estimated_tokens_saved) << " tokens";
                    state.conversation.push_back({"system", oss.str(), false});
                    clamp_chat_focus();
                }
                screen.PostEvent(Event::Custom);

                // Check if micro-compact was sufficient
                if (!should_auto_compact(agent_loop.messages(), config.context_window, token_tracker.last_prompt_tokens())) {
                    return true;
                }
            }
        }

        // Phase 2: Full compact
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.conversation.push_back({"system", "[Auto-compact] Context approaching limit, compacting...", false});
            clamp_chat_focus();
        }
        screen.PostEvent(Event::Custom);

        auto result = compact_context(*provider, agent_loop, state, 4, true);

        if (result.performed) {
            compact_tracking.consecutive_failures = 0;
            compact_tracking.compacted = true;
        } else {
            compact_tracking.consecutive_failures++;
        }

        {
            std::lock_guard<std::mutex> lk(state.mu);
            if (!result.performed) {
                state.conversation.push_back({"system", "[Auto-compact] " + result.error, false});
            } else {
                std::ostringstream oss;
                oss << "[Auto-compact] Compacted " << result.messages_compressed
                    << " messages, saved ~"
                    << TokenTracker::format_tokens(result.estimated_tokens_saved) << " tokens";
                state.conversation.push_back({"system", oss.str(), false});
            }
            clamp_chat_focus();
        }
        screen.PostEvent(Event::Custom);
        return result.performed;
    };
    agent_loop.set_callbacks(callbacks);

    // ---- Session manager ----
    SessionManager session_manager;
    session_manager.start_session(working_dir, provider->name(), provider->model());
    agent_loop.set_session_manager(&session_manager);

    // Register session finalization for clean shutdown
    g_session_manager = &session_manager;
    std::atexit(finalize_session_atexit);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    // ---- Handle --resume ----
    if (resume_latest || !resume_session_id.empty()) {
        std::string target_id = resume_session_id;
        std::string resumed_title;
        if (resume_latest) {
            auto sessions = session_manager.list_sessions();
            if (!sessions.empty()) {
                target_id = sessions.front().id;
                resumed_title = sessions.front().title;
            }
        } else if (!target_id.empty()) {
            // 通过 SessionManager 走多 pid 候选挑最新的 meta(daemon + TUI 并发场景),
            // 避免按本进程 pid 拼路径而读不到别的进程或老格式留下的文件。
            auto meta = session_manager.load_session_meta(target_id);
            resumed_title = meta.title;
        }
        if (!target_id.empty()) {
            // 把 session meta 喂给 resolver,让 resume 真正还原 provider+model。
            // resolve_effective_model 会优先用 (meta.provider, meta.model) 从
            // saved_models 找匹配 entry;找不到则构造 ad-hoc entry,name 以
            // "(session:..." 开头,触发我们后面的系统消息提示。
            SessionMeta resumed_meta = session_manager.load_session_meta(target_id);
            if (!resumed_meta.provider.empty() && !resumed_meta.model.empty()) {
                ModelProfile resumed_entry = resolve_effective_model(
                    config, cwd_override, std::optional<SessionMeta>{resumed_meta});
                swap_provider_if_needed(provider, provider_mu, resumed_entry, config);
                session_manager.set_active_provider(provider->name(), provider->model());
                if (resumed_entry.name.rfind("(session:", 0) == 0) {
                    state.conversation.push_back({"system",
                        "⚠ Resumed with ad-hoc model entry (session recorded " +
                        resumed_meta.provider + "/" + resumed_meta.model +
                        ", not in saved_models). Use /model --default <name> to pick a permanent one.",
                        false});
                }
            }

            auto messages = session_manager.resume_session(target_id);
            acecode::append_resumed_session_messages(messages, state, agent_loop, tools);
            state.conversation.push_back({"system",
                "Resumed session " + target_id + " (" + std::to_string(messages.size()) + " messages)", false});
            if (!resumed_title.empty()) {
                set_terminal_title(resumed_title);
                state.current_session_title = resumed_title;
            }
        } else {
            state.conversation.push_back({"system", "No previous sessions found to resume.", false});
        }
    }

    // Slash command registry
    CommandRegistry cmd_registry;
    register_builtin_commands(cmd_registry);
    {
        auto keys = register_skill_commands_tracked(cmd_registry, skill_registry);
        if (!keys.empty()) {
            LOG_INFO("[skills] Registered " + std::to_string(keys.size()) +
                     " skill slash command(s)");
        }
    }

    // --- Tool progress callbacks (streaming-tool-progress change) ---
    callbacks.on_tool_progress_start = [&state, &screen](
        const std::string& tool_name, const std::string& cmd_preview) {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.tool_running = true;
            state.tool_progress = {};
            state.tool_progress.tool_name = tool_name;
            state.tool_progress.command_preview = cmd_preview;
            state.tool_progress.start_time = std::chrono::steady_clock::now();
            state.last_tool_post_event_time = std::chrono::steady_clock::now();
        }
        screen.PostEvent(Event::Custom);
    };

    callbacks.on_tool_progress_update = [&state, &screen](
        const std::vector<std::string>& tail_snapshot,
        const std::string& current_partial,
        size_t total_bytes, int total_lines) {
        bool should_post = false;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.tool_progress.tail_lines = tail_snapshot;
            state.tool_progress.current_partial = current_partial;
            state.tool_progress.total_bytes = total_bytes;
            state.tool_progress.total_lines = total_lines;
            auto now = std::chrono::steady_clock::now();
            if (now - state.last_tool_post_event_time > std::chrono::milliseconds(150)) {
                state.last_tool_post_event_time = now;
                should_post = true;
            }
        }
        if (should_post) screen.PostEvent(Event::Custom);
    };

    callbacks.on_tool_progress_end = [&state, &screen]() {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.tool_running = false;
            state.tool_progress = {};
        }
        // Unconditional PostEvent so the live element disappears immediately.
        screen.PostEvent(Event::Custom);
    };

    // Now that agent_loop exists, update on_busy_changed to drain pending queue
    callbacks.on_busy_changed = [&state, &clamp_chat_focus, &agent_loop, &screen](bool busy) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (busy && !state.is_waiting) {
            state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
            state.thinking_start_time = std::chrono::steady_clock::now();
            state.streaming_output_chars = 0;
            state.last_completion_tokens_authoritative = 0;
        }
        state.is_waiting = busy;
        if (!busy && !state.pending_queue.empty()) {
            std::string next_prompt = state.pending_queue.front();
            state.pending_queue.erase(state.pending_queue.begin());
            state.conversation.push_back({"user", next_prompt, false});
            // draggable-thick-scrollbar: 用户主动拖滚动条时不要被 worker 线程
            // 强行拽回尾巴 —— 让用户看着自己挑的位置,直到他自己释放鼠标。
            // 拖到底的情况由 clamp_chat_focus 内部 (idx == last) 分支自然恢复。
            if (state.drag_scrollbar_phase ==
                TuiState::DragScrollbarPhase::Idle) {
                state.chat_follow_tail = true;
            }
            clamp_chat_focus();
            state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
            state.thinking_start_time = std::chrono::steady_clock::now();
            state.streaming_output_chars = 0;
            state.last_completion_tokens_authoritative = 0;
            state.is_waiting = true;
            agent_loop.submit(next_prompt);
        }
        screen.PostEvent(Event::Custom);
    };
    agent_loop.set_callbacks(callbacks);

    // ---- Animation ticker thread ----
    std::atomic<bool> running{true};
    std::thread anim_thread([&running, &anim_tick, &state, &screen, &scroll_chat_by_lines] {
        while (running) {
            // drag-autoscroll: 拖动到边界期间提速到 50ms 唤醒, 其他时间保持 300ms.
            // 每次唤醒前读最新 phase, 避免拖动开始后还得等一个完整的 300ms.
            bool fast_tick = false;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                fast_tick = (state.drag_phase == drag_scroll::Phase::ScrollingUp ||
                             state.drag_phase == drag_scroll::Phase::ScrollingDown);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(fast_tick ? 50 : 300));
            anim_tick++;
            // mouse-selection-copy: clear the "Copied N bytes" confirmation
            // ~2 s after the copy fired. Run before the post-event gate so we
            // always get a final render when the deadline passes.
            bool needs_post = false;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                if (state.status_line_clear_at.time_since_epoch().count() != 0 &&
                    std::chrono::steady_clock::now() >= state.status_line_clear_at) {
                    state.status_line = state.status_line_saved;
                    state.status_line_saved.clear();
                    state.status_line_clear_at = {};
                    needs_post = true;
                }

                // drag-autoscroll: 时间门到点就滚一行, 把 ShiftSelection 的补偿请求
                // 累加到 pending_shift_dy, 由事件线程 CatchEvent 的入口消费 — 避免
                // anim_thread 直接改 FTXUI selection_data_ 与 HandleSelection 撞车.
                if (state.drag_phase == drag_scroll::Phase::ScrollingUp ||
                    state.drag_phase == drag_scroll::Phase::ScrollingDown) {
                    auto now = std::chrono::steady_clock::now();
                    if (drag_scroll::should_tick(now, state.last_drag_scroll_at,
                                                  std::chrono::milliseconds(60))) {
                        int dy = (state.drag_phase == drag_scroll::Phase::ScrollingUp) ? -1 : 1;
                        int actual = scroll_chat_by_lines(dy);
                        if (actual != 0) {
                            // scroll_chat_by_lines(+1) 让 focus 下移一行 → 屏幕内容
                            // 相对上移 actual 行 → 原 anchor 文本屏幕坐标应 -actual.
                            state.pending_shift_dy += -actual;
                            needs_post = true;
                        }
                    }
                }
            }
            // Drive re-render while waiting on LLM OR while a tool is running
            // (so the tool-timer chip in the status bar updates every second),
            // or when we just cleared a pending status confirmation.
            if (needs_post || state.is_waiting || state.tool_running) {
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
    auto input_renderer = Renderer([&state](bool) {
        std::string display_text = state.input_text;
        size_t cursor = state.input_cursor;
        if (cursor > display_text.size()) cursor = display_text.size();
        if (display_text.empty()) {
            return hbox({
                text(" ") | focusCursorBar,
                text("Type your prompt here...") | dim | color(Color::GrayDark),
            });
        }
        return render_wrapped_input_text(display_text, cursor);
    });

    // 把已归一化的 paste 文本插入到 state.input_cursor。短文本 inline，超阈值
    // 或多行折叠为 [Pasted text #N (+M lines)] 并把原文存到 pasted_texts。
    // 调用前必须持有 state.mu。
    auto insert_pasted_text_at_cursor = [&state](const std::string& normalized) {
        if (state.input_cursor > state.input_text.size()) {
            state.input_cursor = state.input_text.size();
        }
        std::string to_insert;
        if (acecode::tui::should_fold_to_placeholder(normalized)) {
            const int id = state.next_paste_id++;
            state.pasted_texts[id] = normalized;
            const int n = acecode::tui::count_newlines(normalized);
            to_insert = acecode::tui::format_placeholder(id, n);
        } else {
            to_insert = normalized;
        }
        state.input_text.insert(state.input_cursor, to_insert);
        state.input_cursor += to_insert.size();
        state.history_index = -1;
    };

    // Wrap with CatchEvent to handle all keyboard input
    auto input_with_esc = CatchEvent(input_renderer, [&state, &screen, &clamp_chat_focus, &auth_done, &cmd_registry, &agent_loop, &provider, &provider_mu, &config, &token_tracker, &permissions, &session_manager, &scroll_chat_by_lines, &chat_box, &scrollbar_box, &message_line_counts, &mcp_manager, &tools, &skill_registry, &memory_registry, &working_dir, &insert_pasted_text_at_cursor](Event event) {
        // drag-autoscroll: 事件线程入口处消费 anim_thread 攒下的 selection 偏移
        // 补偿. 所有对 FTXUI selection_data_ 的写都发生在这条路径上, 跟
        // HandleSelection 串行 — 避免跨线程数据竞争. 不 return, 让事件继续正常处理.
        {
            int dy = 0;
            {
                std::lock_guard<std::mutex> lk(state.mu);
                dy = state.pending_shift_dy;
                state.pending_shift_dy = 0;
            }
            if (dy != 0) {
                screen.ShiftSelection(0, dy);
            }
        }
        // 多行粘贴折叠（fix-multiline-paste-input change）：把所有事件先喂进
        // bracketed paste 状态机。begin marker 进入 in-paste，期间所有事件
        // （含 Return / Tab / 字符 / 内嵌 CSI bytes）都聚合到 buffer 而不下发到
        // 正常 Return / 字符 / 删除 / 方向键 handler；end marker 触发 normalize
        // 后或 inline 插入或折叠成 [Pasted text #N +M lines]。空 paste 直接返回。
        {
            std::unique_lock<std::mutex> lk(state.mu);
            acecode::tui::PasteFeedResult pr = event.is_character()
                ? state.paste_accumulator.feed_character(event.character())
                : state.paste_accumulator.feed_special(event.input());
            if (pr.just_completed && !pr.completed_text.empty()) {
                insert_pasted_text_at_cursor(pr.completed_text);
                refresh_slash_dropdown(state, cmd_registry);
                lk.unlock();
                screen.PostEvent(Event::Custom);
            }
            if (pr.consume) {
                return true;
            }
        }
        if (event == Event::CtrlC) {
            // If an operation is active, Ctrl+C behaves like Escape: cancel the
            // current work instead of entering the exit-confirm prompt.
            {
                std::lock_guard<std::mutex> lk(state.mu);
                if (state.is_compacting) {
                    state.compact_abort_requested.store(true);
                    state.conversation.push_back({"system", "Cancelling compaction...", false});
                    state.chat_follow_tail = true;
                    clamp_chat_focus();
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (state.ask_pending || state.confirm_pending ||
                    state.is_waiting || state.tool_running) {
                    screen.PostEvent(Event::Escape);
                    return true;
                }
                // 已经在确认态再按一次 Ctrl+C —— 不视为确认,保持等用户按 y。
                if (!state.exit_confirm_pending) {
                    state.exit_confirm_pending = true;
                }
            }
            screen.PostEvent(Event::Custom);
            return true;
        }

        // Ctrl+C 退出确认 overlay:state.exit_confirm_pending=true 时拦截
        // 字符键 / Esc / Enter,'y'/'Y' 退出,其它都视为取消。鼠标 / resize
        // / custom 事件不拦截,否则 UI 卡死。Ctrl+C 已在上面单独处理。
        if (state.exit_confirm_pending) {
            const bool is_char = event.is_character();
            const bool is_cancel = (event == Event::Escape || event == Event::Return);
            if (is_char || is_cancel) {
                bool yes = false;
                if (is_char) {
                    const std::string& c = event.character();
                    yes = (c == "y" || c == "Y");
                }
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.exit_confirm_pending = false;
                    if (!yes) {
                        state.conversation.push_back({"system", "Exit cancelled.", false});
                        state.chat_follow_tail = true;
                        clamp_chat_focus();
                    }
                }
                if (yes) {
                    screen.Exit();
                } else {
                    screen.PostEvent(Event::Custom);
                }
                return true;
            }
            // 非字符且非 Esc/Enter:不拦截(让鼠标 / 方向键等正常处理)。
        }

        // AskUserQuestion overlay guard:active 时抢占键盘,所有非导航键被吞掉,
        // 不透传到输入框。优先级高于 confirm、slash-dropdown、Return/Esc 等分支。
        // "Other" 自定义文本态下让字符输入 / Backspace 继续走默认路径,只拦截
        // Return / Escape / 方向键。
        {
            std::unique_lock<std::mutex> lk(state.mu);
            if (state.ask_pending) {
                auto& q = state.ask_questions[state.ask_current_question];
                const int option_count = static_cast<int>(q.options.size());
                const int total_rows = option_count + 1; // + "Other..."

                // Esc —— 整体拒绝。
                if (event == Event::Escape) {
                    if (state.ask_other_input_active) {
                        // 先退出 Other 文本模式,保留用户之前的选择。
                        state.ask_other_input_active = false;
                        state.input_text.clear(); state.pasted_texts.clear();
                        state.input_cursor = 0;
                    } else {
                        state.ask_result_ok = false;
                        state.ask_pending = false;
                        state.ask_cv.notify_one();
                    }
                    screen.PostEvent(Event::Custom);
                    return true;
                }

                if (state.ask_other_input_active) {
                    if (event == Event::Return) {
                        std::string answer = state.input_text;
                        state.input_text.clear(); state.pasted_texts.clear();
                        state.input_cursor = 0;
                        state.ask_other_input_active = false;
                        state.ask_result_answers[q.question] = answer;

                        // 推进到下一题或提交。
                        state.ask_current_question++;
                        if (state.ask_current_question >=
                            static_cast<int>(state.ask_questions.size())) {
                            state.ask_result_ok = true;
                            state.ask_pending = false;
                            state.ask_cv.notify_one();
                        } else {
                            state.ask_option_focus = 0;
                            state.ask_multi_selected.assign(
                                state.ask_questions[state.ask_current_question]
                                    .options.size(), false);
                        }
                        screen.PostEvent(Event::Custom);
                        return true;
                    }
                    // Other 输入态:委托 try_handle_ask_other_input 内联处理字符 /
                    // Backspace / Delete / 方向键 / Home / End。helper 返回 true
                    // 表示真的改了 state,我们才 PostEvent 请求重绘 —— Custom /
                    // Mouse 等未识别事件返回 false,**不能** PostEvent,否则
                    // "Custom → swallow → PostEvent(Custom)" 会形成事件自回环
                    // 把事件循环打爆(表现为 TUI 卡死)。无论哪种情况都 return
                    // true 消耗事件,防止透传到下游 shell-mode / slash-dropdown
                    // / Ctrl+E tool_result-expand 等 handler。
                    if (acecode::try_handle_ask_other_input(state, event)) {
                        screen.PostEvent(Event::Custom);
                    }
                    return true;
                }

                // 方向键 / j k 上下移动焦点。
                if (event == Event::ArrowUp ||
                    event == Event::Character('k')) {
                    state.ask_option_focus =
                        (state.ask_option_focus - 1 + total_rows) % total_rows;
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (event == Event::ArrowDown ||
                    event == Event::Character('j')) {
                    state.ask_option_focus =
                        (state.ask_option_focus + 1) % total_rows;
                    screen.PostEvent(Event::Custom);
                    return true;
                }

                // Space —— 仅 multi-select 下对当前焦点项切换勾选;焦点落在
                // "Other..." 行时 Space 不作响应(Other 需要 Enter 进入文本态)。
                if (event == Event::Character(' ')) {
                    if (q.multi_select && state.ask_option_focus < option_count) {
                        if (static_cast<int>(state.ask_multi_selected.size()) <=
                            state.ask_option_focus) {
                            state.ask_multi_selected.resize(option_count, false);
                        }
                        state.ask_multi_selected[state.ask_option_focus] =
                            !state.ask_multi_selected[state.ask_option_focus];
                        screen.PostEvent(Event::Custom);
                    }
                    return true;
                }

                // Enter —— 提交当前题目。
                if (event == Event::Return) {
                    // 焦点在 "Other..." 行:进入自定义文本输入态。
                    if (state.ask_option_focus == option_count) {
                        state.ask_other_input_active = true;
                        state.input_text.clear(); state.pasted_texts.clear();
                        state.input_cursor = 0;
                        screen.PostEvent(Event::Custom);
                        return true;
                    }

                    std::string answer;
                    if (q.multi_select) {
                        for (int i = 0; i < option_count; ++i) {
                            if (i < static_cast<int>(state.ask_multi_selected.size()) &&
                                state.ask_multi_selected[i]) {
                                if (!answer.empty()) answer += ", ";
                                answer += q.options[i].label;
                            }
                        }
                        // 允许空选 —— 上游 schema 没强制,把空字符串交回给模型。
                    } else {
                        answer = q.options[state.ask_option_focus].label;
                    }
                    state.ask_result_answers[q.question] = answer;

                    // 推进或提交。
                    state.ask_current_question++;
                    if (state.ask_current_question >=
                        static_cast<int>(state.ask_questions.size())) {
                        state.ask_result_ok = true;
                        state.ask_pending = false;
                        state.ask_cv.notify_one();
                    } else {
                        state.ask_option_focus = 0;
                        state.ask_multi_selected.assign(
                            state.ask_questions[state.ask_current_question]
                                .options.size(), false);
                    }
                    screen.PostEvent(Event::Custom);
                    return true;
                }

                // 其它键一律吞掉 —— 不让字符进入 input_text(避免破坏下一次
                // "Other" 文本模式的初始状态)。
                return true;
            }
        }

        // Tool-confirmation overlay guard。confirm_pending=true 时键盘归 overlay
        // 所有 —— 上下方向移焦点 / 1-3 数字直选 / Enter 提交 / Shift+Tab 直选
        // option2 / Esc → Deny。其他键(字符 / Backspace / Tab 等)一律吞掉,
        // 防止误打字符串去到 input_text。优先级仅次于 ask_pending(二者互斥)。
        {
            std::unique_lock<std::mutex> lk(state.mu);
            if (state.confirm_pending) {
                auto submit = [&](PermissionResult r) {
                    state.input_text.clear(); state.pasted_texts.clear();
                    state.input_cursor = 0;
                    state.confirm_result = r;
                    state.confirm_pending = false;
                    state.confirm_cv.notify_one();
                    screen.PostEvent(Event::Custom);
                };

                if (event == Event::Escape) {
                    submit(PermissionResult::Deny);
                    return true;
                }
                if (event == Event::ArrowUp) {
                    state.confirm_focus = (state.confirm_focus + 2) % 3;
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (event == Event::ArrowDown) {
                    state.confirm_focus = (state.confirm_focus + 1) % 3;
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (event == Event::TabReverse) {
                    // Shift+Tab 直选 option2(AlwaysAllow),与 overlay 文案中
                    // "(shift+tab)" 提示一致。
                    submit(PermissionResult::AlwaysAllow);
                    return true;
                }
                if (event == Event::Return) {
                    int f = std::clamp(state.confirm_focus, 0, 2);
                    PermissionResult r = (f == 0) ? PermissionResult::Allow
                                       : (f == 1) ? PermissionResult::AlwaysAllow
                                                  : PermissionResult::Deny;
                    submit(r);
                    return true;
                }
                if (event.is_character()) {
                    const std::string& c = event.character();
                    if (c == "1") { submit(PermissionResult::Allow);       return true; }
                    if (c == "2") { submit(PermissionResult::AlwaysAllow); return true; }
                    if (c == "3") { submit(PermissionResult::Deny);        return true; }
                    // y/n 兼容老用户的肌肉记忆;也保留 'a' = always。
                    if (c == "y" || c == "Y") { submit(PermissionResult::Allow);       return true; }
                    if (c == "n" || c == "N") { submit(PermissionResult::Deny);        return true; }
                    if (c == "a" || c == "A") { submit(PermissionResult::AlwaysAllow); return true; }
                    // 其它字符吞掉,不让进入 input_text。
                    return true;
                }
                // 鼠标 / resize / custom 事件不拦截,让默认 handler 处理。
            }
        }

        // Rewind picker guard: target selection first, optional restore-mode
        // selection second. This owns navigation while active.
        {
            std::unique_lock<std::mutex> lk(state.mu);
            if (state.rewind_picker_active) {
                auto clear_picker = [&]() {
                    state.rewind_picker_active = false;
                    state.rewind_mode_active = false;
                    state.rewind_items.clear();
                    state.rewind_selected = 0;
                    state.rewind_view_offset = 0;
                    state.rewind_modes.clear();
                    state.rewind_mode_selected = 0;
                    state.rewind_callback = nullptr;
                };
                auto populate_modes = [&](const TuiState::RewindItem& item) {
                    state.rewind_modes.clear();
                    if (item.can_restore_code) {
                        state.rewind_modes.push_back({
                            TuiState::RewindRestoreMode::CodeAndConversation,
                            "Code and conversation",
                            "Restore tracked files, fork the conversation, and prefill the selected prompt."
                        });
                        state.rewind_modes.push_back({
                            TuiState::RewindRestoreMode::ConversationOnly,
                            "Conversation only",
                            "Fork the conversation and prefill the selected prompt."
                        });
                        state.rewind_modes.push_back({
                            TuiState::RewindRestoreMode::CodeOnly,
                            "Code only",
                            "Restore tracked files without changing the conversation."
                        });
                    } else {
                        state.rewind_modes.push_back({
                            TuiState::RewindRestoreMode::ConversationOnly,
                            "Conversation only",
                            "Fork the conversation and prefill the selected prompt."
                        });
                    }
                    state.rewind_modes.push_back({
                        TuiState::RewindRestoreMode::NeverMind,
                        "Never mind",
                        "Cancel rewind."
                    });
                    state.rewind_mode_selected = 0;
                };
                auto commit_mode = [&](TuiState::RewindRestoreMode mode) {
                    if (state.rewind_selected < 0 ||
                        state.rewind_selected >= static_cast<int>(state.rewind_items.size())) {
                        clear_picker();
                        screen.PostEvent(Event::Custom);
                        return;
                    }
                    auto item = state.rewind_items[state.rewind_selected];
                    auto cb = state.rewind_callback;
                    state.rewind_picker_active = false;
                    state.rewind_mode_active = false;
                    state.rewind_items.clear();
                    state.rewind_selected = 0;
                    state.rewind_view_offset = 0;
                    state.rewind_modes.clear();
                    state.rewind_mode_selected = 0;
                    state.rewind_callback = nullptr;
                    state.input_text.clear(); state.pasted_texts.clear();
                    state.input_cursor = 0;
                    if (cb) cb(std::move(item), mode);
                    clamp_chat_focus();
                    screen.PostEvent(Event::Custom);
                };

                if (event == Event::Escape) {
                    if (state.rewind_mode_active) {
                        state.rewind_mode_active = false;
                        state.rewind_modes.clear();
                        state.rewind_mode_selected = 0;
                    } else {
                        clear_picker();
                        state.conversation.push_back({"system", "Rewind cancelled.", false});
                        state.chat_follow_tail = true;
                        clamp_chat_focus();
                    }
                    screen.PostEvent(Event::Custom);
                    return true;
                }

                const bool move_up =
                    event == Event::ArrowUp || event == Event::Character('k');
                const bool move_down =
                    event == Event::ArrowDown || event == Event::Character('j');
                if (move_up || move_down) {
                    int* selected = state.rewind_mode_active
                        ? &state.rewind_mode_selected
                        : &state.rewind_selected;
                    int count = state.rewind_mode_active
                        ? static_cast<int>(state.rewind_modes.size())
                        : static_cast<int>(state.rewind_items.size());
                    if (count > 0) {
                        if (move_up) {
                            *selected = (*selected - 1 + count) % count;
                        } else {
                            *selected = (*selected + 1) % count;
                        }
                    }
                    if (!state.rewind_mode_active) {
                        state.rewind_view_offset = acecode::tui::scroll_to_keep_visible(
                            state.rewind_selected, state.rewind_view_offset,
                            acecode::tui::kRewindPickerVisibleRows,
                            static_cast<int>(state.rewind_items.size()));
                    }
                    screen.PostEvent(Event::Custom);
                    return true;
                }

                // PgUp/PgDn/Home/End: only the items list (not the 4-row mode
                // list) needs viewport navigation.
                if (!state.rewind_mode_active &&
                    (event == Event::PageUp || event == Event::PageDown ||
                     event == Event::Home || event == Event::End)) {
                    const int total = static_cast<int>(state.rewind_items.size());
                    if (total > 0) {
                        const int step = acecode::tui::kRewindPickerVisibleRows;
                        if (event == Event::PageUp) {
                            state.rewind_selected = std::max(0, state.rewind_selected - step);
                        } else if (event == Event::PageDown) {
                            state.rewind_selected = std::min(total - 1, state.rewind_selected + step);
                        } else if (event == Event::Home) {
                            state.rewind_selected = 0;
                        } else {  // End
                            state.rewind_selected = total - 1;
                        }
                        state.rewind_view_offset = acecode::tui::scroll_to_keep_visible(
                            state.rewind_selected, state.rewind_view_offset, step, total);
                        screen.PostEvent(Event::Custom);
                    }
                    return true;
                }

                auto activate_current_target = [&]() {
                    if (state.rewind_selected < 0 ||
                        state.rewind_selected >= static_cast<int>(state.rewind_items.size())) {
                        return;
                    }
                    const auto& item = state.rewind_items[state.rewind_selected];
                    if (item.can_restore_code) {
                        populate_modes(item);
                        state.rewind_mode_active = true;
                    } else {
                        commit_mode(TuiState::RewindRestoreMode::ConversationOnly);
                    }
                };

                if (event == Event::Return) {
                    if (state.rewind_mode_active) {
                        if (state.rewind_mode_selected >= 0 &&
                            state.rewind_mode_selected < static_cast<int>(state.rewind_modes.size())) {
                            auto mode = state.rewind_modes[state.rewind_mode_selected].mode;
                            commit_mode(mode);
                        }
                    } else {
                        activate_current_target();
                    }
                    screen.PostEvent(Event::Custom);
                    return true;
                }

                if (event.is_character()) {
                    std::string ch = event.character();
                    if (!ch.empty() && ch[0] >= '1' && ch[0] <= '9') {
                        int idx = ch[0] - '1';
                        if (state.rewind_mode_active) {
                            if (idx < static_cast<int>(state.rewind_modes.size())) {
                                state.rewind_mode_selected = idx;
                                auto mode = state.rewind_modes[idx].mode;
                                commit_mode(mode);
                            }
                        } else if (idx < static_cast<int>(state.rewind_items.size())) {
                            state.rewind_selected = idx;
                            activate_current_target();
                        }
                        screen.PostEvent(Event::Custom);
                    }
                    return true;
                }

                return true;
            }
        }

        // Slash-command dropdown guard: when the dropdown is open, intercept
        // the navigation/commit keys before any other overlay or input-history
        // handler gets them.
        {
            std::unique_lock<std::mutex> lk(state.mu);
            if (state.slash_dropdown_active && !state.slash_dropdown_items.empty()) {
                auto commit_selection = [&]() {
                    const auto& item = state.slash_dropdown_items[state.slash_dropdown_selected];
                    state.input_text = "/" + item.name + " ";
                    state.input_cursor = state.input_text.size();
                    state.slash_dropdown_active = false;
                    state.slash_dropdown_items.clear();
                    state.slash_dropdown_selected = 0;
                    state.slash_dropdown_view_offset = 0;
                    state.slash_dropdown_total_matches = 0;
                    state.slash_dropdown_dismissed_for_input = false;
                };
                const int n = static_cast<int>(state.slash_dropdown_items.size());
                auto follow_view = [&]() {
                    state.slash_dropdown_view_offset =
                        acecode::tui::scroll_to_keep_visible(
                            state.slash_dropdown_selected,
                            state.slash_dropdown_view_offset,
                            acecode::tui::kSlashDropdownVisibleRows, n);
                };
                if (event == Event::ArrowUp ||
                    event == Event::Special(std::string(1, '\x10'))) { // Ctrl+P
                    state.slash_dropdown_selected =
                        (state.slash_dropdown_selected - 1 + n) % n;
                    follow_view();
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (event == Event::ArrowDown ||
                    event == Event::Special(std::string(1, '\x0E'))) { // Ctrl+N
                    state.slash_dropdown_selected =
                        (state.slash_dropdown_selected + 1) % n;
                    follow_view();
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (event == Event::PageUp || event == Event::PageDown ||
                    event == Event::Home || event == Event::End) {
                    const int step = acecode::tui::kSlashDropdownVisibleRows;
                    if (event == Event::PageUp) {
                        state.slash_dropdown_selected =
                            std::max(0, state.slash_dropdown_selected - step);
                    } else if (event == Event::PageDown) {
                        state.slash_dropdown_selected =
                            std::min(n - 1, state.slash_dropdown_selected + step);
                    } else if (event == Event::Home) {
                        state.slash_dropdown_selected = 0;
                    } else {  // End
                        state.slash_dropdown_selected = n - 1;
                    }
                    follow_view();
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (event == Event::Return || event == Event::Tab) {
                    commit_selection();
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (event == Event::Escape) {
                    state.slash_dropdown_active = false;
                    state.slash_dropdown_items.clear();
                    state.slash_dropdown_selected = 0;
                    state.slash_dropdown_view_offset = 0;
                    state.slash_dropdown_total_matches = 0;
                    state.slash_dropdown_dismissed_for_input = true;
                    screen.PostEvent(Event::Custom);
                    return true;
                }
            }
        }

        // Enter → submit message
        if (event == Event::Return) {
            std::unique_lock<std::mutex> lk(state.mu);

            // Handle resume picker: Enter confirms selection
            if (state.resume_picker_active) {
                if (state.resume_selected >= 0 &&
                    state.resume_selected < static_cast<int>(state.resume_items.size())) {
                    auto sid = state.resume_items[state.resume_selected].id;
                    auto cb = state.resume_callback;
                    state.resume_picker_active = false;
                    state.resume_items.clear();
                    state.resume_view_offset = 0;
                    state.resume_callback = nullptr;
                    state.input_text.clear(); state.pasted_texts.clear();
                    state.input_cursor = 0;
                    if (cb) cb(sid);
                    clamp_chat_focus();
                }
                screen.PostEvent(Event::Custom);
                return true;
            }

            // confirm_pending 现在由上面的 confirm overlay handler 单独拦截
            // (Enter 直接走那条路径),这里不会再被 confirm 触发。

            if (state.input_text.empty()) return true;
            if (!auth_done) return true;

            // Block message submission during compaction
            if (state.is_compacting) return true;

            // 多行粘贴折叠（fix-multiline-paste-input change）：把已知 [Pasted text #N]
            // 占位符替换回 pasted_texts 中存的全文，得到 expanded_prompt 喂给 agent_loop
            // / 斜杠命令 / pending 队列 / input_history / 对话气泡。提交后清空 input_text
            // 与 pasted_texts；next_paste_id 不复位（display 用计数，跨提交单调递增
            // 即可，避免极端情况下与 input_history 中残留占位符同号撞 ID）。
            //
            // 用户反馈：提交后对话气泡也显示展开版原文，而非紧凑占位符——所以这里
            // 没有保留单独的 `visible_prompt`，提交即一并展开上屏。
            const std::string expanded_prompt = acecode::tui::expand_placeholders(
                state.input_text, state.pasted_texts);
            state.input_text.clear(); state.pasted_texts.clear();
            state.input_cursor = 0;
            refresh_slash_dropdown(state, cmd_registry);

            // 统一入口：内存 push + 磁盘 append。空白 / 相邻重复被抑制，保持磁盘与内存
            // 行为一致；磁盘持久化受 config.input_history.enabled 控制。
            auto record_history = [&state, &config, &working_dir](const std::string& entry) {
                auto is_all_space = [](const std::string& s) {
                    for (unsigned char c : s) {
                        if (!std::isspace(c)) return false;
                    }
                    return true;
                };
                if (entry.empty() || is_all_space(entry)) return;
                if (!state.input_history.empty() && state.input_history.back() == entry) return;
                state.input_history.push_back(entry);
                if (config.input_history.enabled) {
                    std::string path = InputHistoryStore::file_path(
                        SessionStorage::get_project_dir(working_dir));
                    InputHistoryStore::append(path, entry, config.input_history.max_entries);
                }
            };

            // Shell input mode: dispatch directly to BashTool via agent worker.
            // Skips slash-command parsing and LLM round-trip.
            if (state.input_mode == InputMode::Shell) {
                const std::string shell_cmd = expanded_prompt;
                record_history(prepend_mode_prefix(shell_cmd, InputMode::Shell));
                state.history_index = -1;
                state.input_mode = InputMode::Normal;

                // 提交后对话气泡显示展开后的原文（用户反馈：上屏不要看到 [] 占位符）。
                state.conversation.push_back({"user", "!" + shell_cmd, false});
                state.chat_follow_tail = true;
                clamp_chat_focus();
                state.current_thinking_phrase = "Running shell";
                state.thinking_start_time = std::chrono::steady_clock::now();
                state.streaming_output_chars = 0;
                state.last_completion_tokens_authoritative = 0;
                state.is_waiting = true;
                agent_loop.submit_shell(shell_cmd);
                return true;
            }

            // Record history（用 expanded_prompt：上箭头取回原文，再次提交不会发出
            // 字面 [Pasted text #N] —— 因为本会话提交后 store 已经清空，未展开的
            // 字面占位符在下次 submit 时也会按 unknown id 保留，丢失原文。）
            record_history(expanded_prompt);
            state.history_index = -1;

            // Slash command interception（用 expanded_prompt 派发：spec 4.3）。
            if (!expanded_prompt.empty() && expanded_prompt[0] == '/') {
                CommandContext cmd_ctx{
                    state, agent_loop, *provider, &provider, &provider_mu,
                    config, token_tracker,
                    permissions,
                    [&screen]() { screen.Exit(); },
                    &session_manager,
                    [&screen]() { screen.PostEvent(Event::Custom); },
                    &mcp_manager,
                    &tools,
                    &skill_registry,
                    &memory_registry,
                    &cmd_registry,
                    working_dir
                };
                lk.unlock();
                bool handled = cmd_registry.dispatch(expanded_prompt, cmd_ctx);
                if (handled) {
                    lk.lock();
                    clamp_chat_focus();
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                lk.lock();
                // If not a known command, fall through to send as normal prompt
            }

            if (state.is_waiting) {
                // 队列保存展开版（spec 4.5）；提交后气泡显示展开版（用户反馈：
                // 上屏不要看到 [] 占位符）。
                state.pending_queue.push_back(expanded_prompt);
                state.conversation.push_back({"user", expanded_prompt, false});
                state.chat_follow_tail = true;
                clamp_chat_focus();
            } else {
                state.conversation.push_back({"user", expanded_prompt, false});
                state.chat_follow_tail = true;
                clamp_chat_focus();
                state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
                state.thinking_start_time = std::chrono::steady_clock::now();
                state.streaming_output_chars = 0;
                state.last_completion_tokens_authoritative = 0;
                state.is_waiting = true;
                agent_loop.submit(expanded_prompt);
            }
            return true;
        }
        // /resume picker viewport navigation: PgUp/PgDn jump a full window,
        // Home/End jump to first/last item. Must short-circuit before chat
        // scroll handlers so the picker absorbs these keys exclusively.
        if (event == Event::PageUp || event == Event::PageDown ||
            event == Event::Home || event == Event::End) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.resume_picker_active) {
                const int total = static_cast<int>(state.resume_items.size());
                if (total > 0) {
                    const int step = acecode::tui::kResumePickerVisibleRows;
                    if (event == Event::PageUp) {
                        state.resume_selected = std::max(0, state.resume_selected - step);
                    } else if (event == Event::PageDown) {
                        state.resume_selected = std::min(total - 1, state.resume_selected + step);
                    } else if (event == Event::Home) {
                        state.resume_selected = 0;
                    } else {  // End
                        state.resume_selected = total - 1;
                    }
                    state.resume_view_offset = acecode::tui::scroll_to_keep_visible(
                        state.resume_selected, state.resume_view_offset, step, total);
                    screen.PostEvent(Event::Custom);
                }
                return true;
            }
        }
        if (event == Event::PageUp) {
            std::lock_guard<std::mutex> lk(state.mu);
            int height = chat_box.y_max - chat_box.y_min + 1;
            int step = height > 3 ? height - 2 : 1;  // 留 2 行重叠
            if (scroll_chat_by_lines(-step) != 0) {
                screen.PostEvent(Event::Custom);
            }
            return true;
        }
        if (event == Event::PageDown) {
            std::lock_guard<std::mutex> lk(state.mu);
            int height = chat_box.y_max - chat_box.y_min + 1;
            int step = height > 3 ? height - 2 : 1;
            if (scroll_chat_by_lines(step) != 0) {
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
            // drag-autoscroll: Esc 中止任何进行中的拖动自动滚动. 选区本身由
            // 下游 FTXUI 通过 `handled=true` 情况下的 HandleSelection 清空
            // (如果之前有选择). 我们只负责把自己的状态机拉回 Idle.
            if (state.drag_left_pressed ||
                state.drag_phase != drag_scroll::Phase::Idle) {
                state.drag_left_pressed = false;
                state.drag_phase = drag_scroll::Phase::Idle;
                state.last_drag_scroll_at = {};
            }
            // Escape during resume picker → cancel
            if (state.resume_picker_active) {
                state.resume_picker_active = false;
                state.resume_items.clear();
                state.resume_view_offset = 0;
                state.resume_callback = nullptr;
                state.input_text.clear(); state.pasted_texts.clear();
                state.input_cursor = 0;
                state.conversation.push_back({"system", "Resume cancelled.", false});
                state.chat_follow_tail = true;
                clamp_chat_focus();
                screen.PostEvent(Event::Custom);
                return true;
            }
            // confirm_pending 的 Esc → Deny 已由上面的 confirm overlay handler
            // 拦截,这里不再重复处理。

            // Shell mode: Escape exits and clears the buffer. Takes precedence
            // over `is_waiting` abort so typing Esc in shell mode never fires
            // an unintended cancel on a pending agent turn.
            if (state.input_mode == InputMode::Shell) {
                state.input_mode = InputMode::Normal;
                state.input_text.clear(); state.pasted_texts.clear();
                state.input_cursor = 0;
                return true;
            }
            if (state.is_waiting || state.tool_running) {
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
            auto& mouse = event.mouse();

            // mouse-selection-copy: right-click-press copies the current
            // FTXUI selection to the terminal clipboard via OSC 52. Runs
            // anywhere inside the TUI (not gated by chat_box) so users can
            // right-click wherever feels natural. Silent no-op when the
            // selection is empty.
            if (mouse.button == Mouse::Right && mouse.motion == Mouse::Pressed) {
                std::string sel = screen.GetSelection();
                if (sel.empty()) {
                    return false;
                }
                std::string seq = "\x1b]52;c;" + base64_encode(sel) + "\x1b\\";
                std::fwrite(seq.data(), 1, seq.size(), stdout);
                std::fflush(stdout);
                LOG_INFO("Copied " + std::to_string(sel.size()) +
                         " bytes to clipboard via OSC 52");
                {
                    std::lock_guard<std::mutex> lk(state.mu);
                    // sel.size() is a byte count, not a codepoint count — the
                    // number shown matches what OSC 52 actually transports.
                    std::string msg = "Copied " + std::to_string(sel.size()) +
                                      " bytes to clipboard";
                    // Snapshot the prior status so we can restore it on clear.
                    if (state.status_line_clear_at.time_since_epoch().count() == 0) {
                        state.status_line_saved = state.status_line;
                    }
                    state.status_line = msg;
                    state.status_line_clear_at =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
                }
                screen.PostEvent(Event::Custom);
                return true;
            }

            // draggable-thick-scrollbar: 左键 Pressed 落在滚动条列时,优先
            // 进入"拖滚动条"分支,跳过下面的 drag-select 启动。两态互斥 ——
            // 一次按下不会同时开始选区拖拽和滚动条拖拽。
            if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed &&
                scrollbar_box.Contain(mouse.x, mouse.y)) {
                std::lock_guard<std::mutex> lk(state.mu);
                // 快照 message_line_counts —— 流式输出追加新行时,拖动期间的
                // y → line 映射继续按按下瞬间的几何走,不被指针下扯走。
                state.drag_scrollbar_snapshot = message_line_counts;
                state.drag_scrollbar_phase = TuiState::DragScrollbarPhase::Dragging;
                // 一旦用户主动操纵滚动条,就视为离开尾巴跟随;松手时不自动恢复。
                state.chat_follow_tail = false;
                int track_height = scrollbar_box.y_max - scrollbar_box.y_min + 1;
                auto [idx, off] = acecode::tui::y_to_focus(
                    mouse.y, scrollbar_box.y_min, track_height,
                    state.drag_scrollbar_snapshot);
                if (idx >= 0) {
                    state.chat_focus_index = idx;
                    state.chat_line_offset = off;
                    int last_msg = static_cast<int>(state.conversation.size()) - 1;
                    if (idx == last_msg) {
                        // 拖到底自然恢复 follow_tail —— 与 scroll_chat 的
                        // (next == last) 路径保持一致。
                        state.chat_follow_tail = true;
                    }
                }
                screen.PostEvent(Event::Custom);
                return true;
            }

            // drag-autoscroll: 跟踪左键按下/拖动/释放, 驱动 anim_thread 在用户
            // 把鼠标拖到 chat_box 顶部/底部时自动滚动并补偿 selection 坐标.
            // 不 return true, 让事件继续流向 FTXUI 的 HandleSelection 走原本的
            // 选区更新. Release/Moved 不做 chat_box.Contain 限制, 因为用户常常
            // 会把鼠标拖到窗口外松开, 此时我们仍要把状态机拉回 Idle.
            if (mouse.button == Mouse::Left) {
                if (mouse.motion == Mouse::Pressed) {
                    // draggable-thick-scrollbar: 落在滚动条列上的 Pressed 已经
                    // 在上一个分支被吞掉,这里只剩内容区的左键按下 → 启动选区拖拽。
                    if (chat_box.Contain(mouse.x, mouse.y) &&
                        !scrollbar_box.Contain(mouse.x, mouse.y)) {
                        std::lock_guard<std::mutex> lk(state.mu);
                        state.drag_left_pressed = true;
                        state.last_mouse_x = mouse.x;
                        state.last_mouse_y = mouse.y;
                        state.drag_phase = drag_scroll::Phase::Dragging;
                        state.last_drag_scroll_at = {};
                    }
                } else if (mouse.motion == Mouse::Released) {
                    std::lock_guard<std::mutex> lk(state.mu);
                    state.drag_left_pressed = false;
                    state.drag_phase = drag_scroll::Phase::Idle;
                    state.last_drag_scroll_at = {};
                    // draggable-thick-scrollbar: 任何左键 Released 都把滚动条
                    // 拖拽态拉回 Idle,即使释放点已经离开滚动条列(用户经常
                    // 拖出窗口外松开)。chat_follow_tail 不在这里恢复 —— 用户
                    // 显式滚到了非尾位置,只在拖到底时由 Pressed 分支重新开启。
                    state.drag_scrollbar_phase = TuiState::DragScrollbarPhase::Idle;
                    state.drag_scrollbar_snapshot.clear();
                }
            }
            // 终端在拖动期间发 Moved 事件, button 字段通常是 None 而不是 Left.
            // 我们靠自己维护的 drag_left_pressed 判断是否处于拖动中.
            if (mouse.motion == Mouse::Moved) {
                std::lock_guard<std::mutex> lk(state.mu);
                // draggable-thick-scrollbar: 滚动条拖拽优先级高于 drag-select
                // —— 两态互斥,Pressed 分支保证只可能进一态。直接 early return,
                // 不让下面的 drag-autoscroll 分类运行,避免拖滚动条时选区被误激活。
                if (state.drag_scrollbar_phase ==
                    TuiState::DragScrollbarPhase::Dragging) {
                    int track_height =
                        scrollbar_box.y_max - scrollbar_box.y_min + 1;
                    auto [idx, off] = acecode::tui::y_to_focus(
                        mouse.y, scrollbar_box.y_min, track_height,
                        state.drag_scrollbar_snapshot);
                    if (idx >= 0) {
                        state.chat_focus_index = idx;
                        state.chat_line_offset = off;
                        int last_msg =
                            static_cast<int>(state.conversation.size()) - 1;
                        state.chat_follow_tail = (idx == last_msg);
                    }
                    screen.PostEvent(Event::Custom);
                    return true;
                }
                if (state.drag_left_pressed) {
                    state.last_mouse_x = mouse.x;
                    state.last_mouse_y = mouse.y;
                    auto new_phase = drag_scroll::classify(
                        mouse.y, chat_box.y_min, chat_box.y_max, true,
                        drag_scroll::Config{});
                    bool phase_changed = (new_phase != state.drag_phase);
                    state.drag_phase = new_phase;
                    // 进入滚动阶段时立即 PostEvent, 让 anim_thread 尽快转到
                    // 50ms 间隔 + 跑第一次 tick. 不 PostEvent 也会在下个 300ms
                    // 唤醒读到新 phase, 但那个延迟体感很差.
                    if (phase_changed &&
                        (new_phase == drag_scroll::Phase::ScrollingUp ||
                         new_phase == drag_scroll::Phase::ScrollingDown)) {
                        screen.PostEvent(Event::Custom);
                    }
                }
            }

            std::lock_guard<std::mutex> lk(state.mu);
            if (!chat_box.Contain(mouse.x, mouse.y)) {
                return false;
            }

            // 鼠标滚轮按行滚动 (3 行/notch, Win 默认值), 长消息不再被一格掠过。
            constexpr int WHEEL_LINES = 3;
            if (mouse.button == Mouse::WheelUp) {
                if (scroll_chat_by_lines(-WHEEL_LINES) != 0) {
                    screen.PostEvent(Event::Custom);
                }
                return true;
            }
            if (mouse.button == Mouse::WheelDown) {
                if (scroll_chat_by_lines(WHEEL_LINES) != 0) {
                    screen.PostEvent(Event::Custom);
                }
                return true;
            }
        }
        if (event == Event::ArrowUp) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.resume_picker_active) {
                if (state.resume_selected > 0) state.resume_selected--;
                state.resume_view_offset = acecode::tui::scroll_to_keep_visible(
                    state.resume_selected, state.resume_view_offset,
                    acecode::tui::kResumePickerVisibleRows,
                    static_cast<int>(state.resume_items.size()));
                screen.PostEvent(Event::Custom);
                return true;
            }
            if (state.input_history.empty()) return true;
            if (state.history_index == -1) {
                state.saved_input = prepend_mode_prefix(state.input_text, state.input_mode);
                state.history_index = (int)state.input_history.size() - 1;
            } else if (state.history_index > 0) {
                state.history_index--;
            }
            auto [hist_mode, hist_text] = parse_mode_prefix(state.input_history[state.history_index]);
            state.input_mode = hist_mode;
            state.input_text = hist_text;
            state.input_cursor = state.input_text.size();
            // 历史覆盖输入：清掉本会话旧粘贴留下的孤儿 pasted_texts（spec 3.7）。
            acecode::tui::prune_unreferenced(state.pasted_texts, state.input_text);
            refresh_slash_dropdown(state, cmd_registry);
            return true;
        }
        if (event == Event::ArrowDown) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.resume_picker_active) {
                if (state.resume_selected < static_cast<int>(state.resume_items.size()) - 1)
                    state.resume_selected++;
                state.resume_view_offset = acecode::tui::scroll_to_keep_visible(
                    state.resume_selected, state.resume_view_offset,
                    acecode::tui::kResumePickerVisibleRows,
                    static_cast<int>(state.resume_items.size()));
                screen.PostEvent(Event::Custom);
                return true;
            }
            if (state.history_index == -1) return true;
            if (state.history_index < (int)state.input_history.size() - 1) {
                state.history_index++;
                auto [hist_mode, hist_text] = parse_mode_prefix(state.input_history[state.history_index]);
                state.input_mode = hist_mode;
                state.input_text = hist_text;
            } else {
                state.history_index = -1;
                auto [saved_mode, saved_text] = parse_mode_prefix(state.saved_input);
                state.input_mode = saved_mode;
                state.input_text = saved_text;
            }
            state.input_cursor = state.input_text.size();
            // 历史覆盖输入：清掉本会话旧粘贴留下的孤儿 pasted_texts（spec 3.7）。
            acecode::tui::prune_unreferenced(state.pasted_texts, state.input_text);
            refresh_slash_dropdown(state, cmd_registry);
            return true;
        }
        // ArrowLeft / ArrowRight: move caret one UTF-8 glyph.
        if (event == Event::ArrowLeft) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.resume_picker_active) return true;
            if (state.input_cursor > state.input_text.size()) {
                state.input_cursor = state.input_text.size();
            }
            if (state.input_cursor == 0) return true;
            // 已知 [Pasted text #N] 占位符整体跨越（atomic span）。
            if (auto span = acecode::tui::placeholder_ending_at(
                    state.input_text, state.pasted_texts, state.input_cursor)) {
                state.input_cursor = span->begin;
                return true;
            }
            size_t pos = state.input_cursor - 1;
            while (pos > 0 && (static_cast<unsigned char>(state.input_text[pos]) & 0xC0) == 0x80) {
                pos--;
            }
            state.input_cursor = pos;
            return true;
        }
        if (event == Event::ArrowRight) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.resume_picker_active) return true;
            if (state.input_cursor >= state.input_text.size()) {
                state.input_cursor = state.input_text.size();
                return true;
            }
            // 已知 [Pasted text #N] 占位符整体跨越（atomic span）。
            if (auto span = acecode::tui::placeholder_starting_at(
                    state.input_text, state.pasted_texts, state.input_cursor)) {
                state.input_cursor = span->end;
                return true;
            }
            size_t pos = state.input_cursor + 1;
            while (pos < state.input_text.size() &&
                   (static_cast<unsigned char>(state.input_text[pos]) & 0xC0) == 0x80) {
                pos++;
            }
            state.input_cursor = pos;
            return true;
        }
        // Home / End: jump caret to start/end of the input buffer.
        // FTXUI only maps `ESC [ H` and `ESC [ F` (plus the `ESC O H/F` DECCKM
        // variants) to Event::Home / Event::End. Several terminals emit VT220-
        // style `ESC [ 1 ~` / `ESC [ 4 ~` or rxvt-style `ESC [ 7 ~` / `ESC [ 8 ~`
        // instead — those arrive as raw Special events, so match them here.
        // Ctrl+A / Ctrl+E are also honoured as the readline-style fallback.
        auto is_home_event = [](const Event& e) {
            return e == Event::Home
                || e == Event::Special("\x1B[1~")
                || e == Event::Special("\x1B[7~")
                || e == Event::Special(std::string(1, '\x01')); // Ctrl+A
        };
        auto is_end_event = [](const Event& e) {
            return e == Event::End
                || e == Event::Special("\x1B[4~")
                || e == Event::Special("\x1B[8~")
                || e == Event::Special(std::string(1, '\x05')); // Ctrl+E
        };
        if (is_home_event(event)) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.resume_picker_active) return true;
            state.input_cursor = 0;
            return true;
        }
        // Ctrl+E contextual expand: when a summarized tool_result is focused
        // in the chat view, toggle its expanded state. Falls through to the
        // readline-style "move to end of line" when no chat message is focused.
        if (event == Event::Special(std::string(1, '\x05'))) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.chat_focus_index >= 0 &&
                state.chat_focus_index < static_cast<int>(state.conversation.size())) {
                auto& msg = state.conversation[state.chat_focus_index];
                if (msg.role == "tool_result" &&
                    (msg.summary.has_value() || msg.hunks.has_value())) {
                    msg.expanded = !msg.expanded;
                    screen.PostEvent(Event::Custom);
                    return true;
                }
            }
            // Fall through to end-of-line if nothing to toggle.
        }
        if (is_end_event(event)) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.resume_picker_active) return true;
            state.input_cursor = state.input_text.size();
            return true;
        }
        // Delete: remove UTF-8 glyph at the caret (to the right)
        if (event == Event::Delete) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.input_cursor > state.input_text.size()) {
                state.input_cursor = state.input_text.size();
            }
            if (state.input_cursor >= state.input_text.size()) return true;
            // 已知 [Pasted text #N] 占位符整体删除：连同 store 条目一起回收。
            if (auto span = acecode::tui::placeholder_starting_at(
                    state.input_text, state.pasted_texts, state.input_cursor)) {
                state.pasted_texts.erase(span->paste_id);
                state.input_text.erase(span->begin, span->end - span->begin);
                // input_cursor 保持在 span->begin（即 input_cursor 不变）
                refresh_slash_dropdown(state, cmd_registry);
                return true;
            }
            size_t next = state.input_cursor + 1;
            while (next < state.input_text.size() &&
                   (static_cast<unsigned char>(state.input_text[next]) & 0xC0) == 0x80) {
                next++;
            }
            state.input_text.erase(state.input_cursor, next - state.input_cursor);
            refresh_slash_dropdown(state, cmd_registry);
            return true;
        }
        // Backspace: remove UTF-8 glyph preceding the caret
        if (event == Event::Backspace) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.input_cursor > state.input_text.size()) {
                state.input_cursor = state.input_text.size();
            }
            if (state.input_text.empty()) {
                // On empty buffer, Backspace exits Shell mode back to Normal.
                if (state.input_mode == InputMode::Shell) {
                    state.input_mode = InputMode::Normal;
                }
                state.input_cursor = 0;
                refresh_slash_dropdown(state, cmd_registry);
                return true;
            }
            if (state.input_cursor == 0) {
                return true;
            }
            // 已知 [Pasted text #N] 占位符整体删除：连同 store 条目一起回收。
            if (auto span = acecode::tui::placeholder_ending_at(
                    state.input_text, state.pasted_texts, state.input_cursor)) {
                state.pasted_texts.erase(span->paste_id);
                state.input_text.erase(span->begin, span->end - span->begin);
                state.input_cursor = span->begin;
                refresh_slash_dropdown(state, cmd_registry);
                return true;
            }
            size_t pos = state.input_cursor - 1;
            // Walk back over UTF-8 continuation bytes (10xxxxxx)
            while (pos > 0 && (static_cast<unsigned char>(state.input_text[pos]) & 0xC0) == 0x80) {
                pos--;
            }
            state.input_text.erase(pos, state.input_cursor - pos);
            state.input_cursor = pos;
            refresh_slash_dropdown(state, cmd_registry);
            return true;
        }
        // Printable character input
        if (event.is_character()) {
            std::lock_guard<std::mutex> lk(state.mu);
            // During resume picker, digit keys select directly
            if (state.resume_picker_active) {
                std::string ch = event.character();
                if (!ch.empty() && ch[0] >= '1' && ch[0] <= '9') {
                    int idx = ch[0] - '1';
                    if (idx < static_cast<int>(state.resume_items.size())) {
                        auto sid = state.resume_items[idx].id;
                        auto cb = state.resume_callback;
                        state.resume_picker_active = false;
                        state.resume_items.clear();
                        state.resume_view_offset = 0;
                        state.resume_callback = nullptr;
                        state.input_text.clear(); state.pasted_texts.clear();
                        state.input_cursor = 0;
                        if (cb) cb(sid);
                        clamp_chat_focus();
                        screen.PostEvent(Event::Custom);
                    }
                }
                return true;
            }
            // Shell-mode trigger: `!` on an empty Normal buffer switches mode
            // without being inserted. Subsequent `!` characters are literal.
            if (state.input_mode == InputMode::Normal &&
                state.input_text.empty() &&
                event.character() == "!") {
                state.input_mode = InputMode::Shell;
                state.history_index = -1;
                return true;
            }
            const std::string ch = event.character();
            if (state.input_cursor > state.input_text.size()) {
                state.input_cursor = state.input_text.size();
            }
            state.input_text.insert(state.input_cursor, ch);
            state.input_cursor += ch.size();
            // Reset history browsing on new input
            state.history_index = -1;
            refresh_slash_dropdown(state, cmd_registry);
            return true;
        }
        return false;
    });

    auto renderer = Renderer(input_with_esc, [&state, &screen, &version_str, &cwd_display, &chat_box, &scrollbar_box, &message_boxes, &message_line_counts, &anim_tick, &input_with_esc, &permissions, dangerous_mode] {
        std::lock_guard<std::mutex> lk(state.mu);

        // drag-autoscroll: 把上一帧 reflect 回填的 box 高度同步到行数表,
        // 供 scroll_chat_by_lines 做按行滚动. 再把 boxes 清零, 这样如果某条
        // 消息本帧因为 yframe 裁剪不被 reflect, line_counts 也能稳定下来.
        size_t n_msgs = state.conversation.size();
        message_line_counts.resize(n_msgs);
        for (size_t i = 0; i < n_msgs; ++i) {
            if (i < message_boxes.size()) {
                int h = message_boxes[i].y_max - message_boxes[i].y_min + 1;
                if (h > 0) {
                    message_line_counts[i] = h;
                } else if (message_line_counts[i] <= 0) {
                    message_line_counts[i] = 1;
                }
            } else if (message_line_counts[i] <= 0) {
                message_line_counts[i] = 1;
            }
        }

        // selection-anchor-compensation: 在清空 boxes 之前,先用上一帧 reflect 的
        // box.y_min 检测 anchor 漂移。focus_index 和 line_offset 都没变(用户没动
        // 滚轮 / PgUp / 拖滚动条)而 y_min 变了 —— 这种纯 layout 漂移期间如果用户
        // 在拖选,FTXUI 的 selection_data_ 钉在物理屏幕坐标会错位,这里调
        // ShiftSelection 把锚点跟随移到新位置。ShiftSelection 内部会在没有 active
        // selection 时 early return,这里无条件调用是安全的。
        {
            int cur_focus = state.chat_focus_index;
            int cur_offset = state.chat_line_offset;
            int cur_y = (cur_focus >= 0 && cur_focus < (int)message_boxes.size())
                ? message_boxes[cur_focus].y_min : 0;
            bool focus_unchanged = (cur_focus == state.last_focus_index &&
                                     cur_offset == state.last_chat_line_offset);
            // last_focus_box_y 的 sentinel 是 -999999 (从未拍过),用 > -1000000
            // 判断"已有有效快照"。cur_y 在 reflect 没回填时是 0(default Box),
            // 也跳过补偿——避免被裁出 viewport 的 anchor 触发假阳性 dy。
            if (focus_unchanged && cur_y > 0 &&
                state.last_focus_box_y > -1000000 &&
                cur_y != state.last_focus_box_y) {
                int dy = cur_y - state.last_focus_box_y;
                screen.ShiftSelection(0, dy);
            }
            // 仅在拿到有效 reflect 数据时更新快照,否则保留上次的;这样 anchor
            // 短暂被裁出 viewport 再回到可见区时,差值仍是相对最近一次可见位置。
            if (cur_y > 0) {
                state.last_focus_box_y = cur_y;
            }
            state.last_focus_index = cur_focus;
            state.last_chat_line_offset = cur_offset;
        }

        message_boxes.assign(n_msgs, Box{});

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
        // drag-autoscroll: 每条消息渲染时 reflect 到 message_boxes[i], 下一帧
        // Renderer 开头用这些 box 的高度更新 message_line_counts (按行滚动需要).
        Elements message_elements;
        for (size_t i = 0; i < state.conversation.size(); ++i) {
            const auto& msg = state.conversation[i];
            bool focused_message = static_cast<int>(i) == state.chat_focus_index;
            Decorator focus_decorator = nothing;
            if (focused_message) {
                // drag-autoscroll: 在焦点消息内用 chat_line_offset/total_lines 的
                // 比例定位到具体某一行. 按行滚动时 ratio 线性变化, yframe 会相应
                // 上下滑动 viewport. total_lines 从上一帧 reflect 回填 (没有数据时
                // fallback 为 1, ratio=0, 行为等同未拖动状态).
                int total_lines = (i < message_line_counts.size() &&
                                   message_line_counts[i] > 0)
                    ? message_line_counts[i] : 1;
                float ratio = 0.0f;
                if (state.chat_follow_tail) {
                    ratio = 1.0f;
                } else if (total_lines > 1) {
                    ratio = static_cast<float>(state.chat_line_offset) /
                            static_cast<float>(total_lines - 1);
                }
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
                focus_decorator = focusPositionRelative(0.0f, ratio);
            }

            if (msg.role == "user") {
                auto line = hbox({
                    text(" > ") | bold | color(Color::Blue),
                    paragraph(msg.content) | color(Color::White) | flex,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator | reflect(message_boxes[i]));
            } else if (msg.role == "assistant") {
                // Render with Markdown formatting
                Element md_content;
                try {
                    acecode::markdown::FormatOptions md_opts;
                    md_opts.terminal_width = ftxui::Terminal::Size().dimx - 6;
                    md_opts.syntax_highlight = true;
                    md_opts.hyperlinks = true;
                    md_opts.strip_xml = true;
                    md_content = acecode::markdown::format_markdown(msg.content, md_opts);
                } catch (...) {
                    // Fallback: raw paragraph if markdown parsing fails
                    md_content = paragraph(msg.content) | color(Color::GreenLight);
                }
                auto line = hbox({
                    text(" * ") | bold | color(Color::Green),
                    md_content | flex,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator | reflect(message_boxes[i]));
            } else if (msg.role == "tool_call") {
                // Prefer the compact one-line preview (→ bash  npm install) when
                // agent_loop supplied one; fall back to the legacy full-JSON row.
                std::string display_text;
                if (!msg.display_override.empty()) {
                    display_text = "\xE2\x86\x92 " + msg.display_override; // "→ "
                } else {
                    display_text = msg.content;
                }
                auto line = hbox({
                    text("   -> ") | color(Color::Magenta),
                    paragraph(display_text) | color(Color::MagentaLight) | dim | flex,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator | reflect(message_boxes[i]));
            } else if (msg.role == "user_shell_output") {
                // 用户主动 `!cmd` 的输出 —— 全量显示,无截断、无摘要、无 fold。
                // 用户自己输入的命令就是想看完整输出,任何折叠都不符合预期。
                // 这与 LLM 工具结果(role="tool_result")形成对照:LLM 调用的
                // 工具结果走摘要/diff/fold 三优先级,有 Ctrl+E 展开机制。
                auto line = hbox({
                    text("   <- ") | color(Color::GrayDark),
                    render_tool_result_lines_preserving_breaks(msg.content) | flex,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator | reflect(message_boxes[i]));
            } else if (msg.role == "tool_result") {
                // 新优先级:有结构化 hunks → 走彩色 diff 视图(summary + 色带);
                // 其次 summary(无 hunks)→ 单行摘要;都没有 → 灰色 fold。
                const bool use_diff = msg.hunks.has_value();
                const bool use_summary = msg.summary.has_value() && !msg.expanded && !use_diff;
                if (use_diff) {
                    // ---- Diff 视图:summary 行 + 彩色 diff 块 ----
                    Elements rows;
                    if (msg.summary.has_value()) {
                        const auto& s = *msg.summary;
                        Color row_color = msg.content.find("[Error]") == 0 || !is_success_summary(s)
                            ? Color::RedLight
                            : Color::GreenLight;
                        std::string metric_str;
                        for (const auto& kv : s.metrics) {
                            std::string seg;
                            if (kv.first == "+") seg = "+" + kv.second;
                            else if (kv.first == "-") seg = "-" + kv.second;
                            else seg = kv.first + "=" + kv.second;
                            if (!metric_str.empty()) metric_str += " \xC2\xB7 ";
                            metric_str += seg;
                        }
                        const int summary_width = std::max(
                            20, chat_box.x_max - chat_box.x_min - 6);
                        std::string summary_line = renderable_tool_summary_line(
                            s, metric_str, summary_width);
                        rows.push_back(hbox({
                            text("   <- ") | color(Color::GrayDark),
                            text(summary_line) | color(row_color) | flex,
                        }));
                    } else {
                        rows.push_back(hbox({
                            text("   <- ") | color(Color::GrayDark),
                            text("diff") | color(Color::GrayLight) | flex,
                        }));
                    }

                    // 失败态:把前 3 行 stderr dim 显示在 summary 之下(保留既有行为)。
                    if (msg.summary.has_value() && !is_success_summary(*msg.summary) &&
                        !msg.content.empty()) {
                        int shown = 0;
                        size_t pos = 0;
                        while (pos < msg.content.size() && shown < 3) {
                            size_t nl = msg.content.find('\n', pos);
                            std::string line = (nl == std::string::npos)
                                ? msg.content.substr(pos)
                                : msg.content.substr(pos, nl - pos);
                            rows.push_back(hbox({
                                text("      ") | color(Color::GrayDark),
                                paragraph(line) | color(Color::GrayLight) | dim | flex,
                            }));
                            if (nl == std::string::npos) break;
                            pos = nl + 1;
                            ++shown;
                        }
                    }

                    // Diff 视图:缩进 6 列,宽度由 chat_box 推导。
                    DiffViewOptions opts;
                    opts.width = std::max(20, chat_box.x_max - chat_box.x_min - 6);
                    opts.expanded = msg.expanded;
                    opts.max_hunks = 3;
                    opts.max_lines_per_hunk = 20;
                    Element diff_el = render_diff_view(*msg.hunks, opts);
                    rows.push_back(hbox({
                        text("      ") | color(Color::GrayDark),
                        diff_el | flex,
                    }));

                    auto block = vbox(std::move(rows));
                    if (focused_message) {
                        block = block | focus;
                    }
                    message_elements.push_back(block | focus_decorator | reflect(message_boxes[i]));
                } else if (use_summary) {
                    // ---- Summary row: single line, icon + verb + object + metrics ----
                    const auto& s = *msg.summary;
                    Color row_color = msg.content.find("[Error]") == 0 || !is_success_summary(s)
                        ? Color::RedLight
                        : Color::GreenLight;

                    // Build metric tail: " · k=v · k=v" but drop k for
                    // "time"/"bytes"/"lines"/"size" since the value is self-describing.
                    std::string metric_str;
                    for (const auto& kv : s.metrics) {
                        std::string seg;
                        if (kv.first == "time" || kv.first == "bytes" ||
                            kv.first == "size" || kv.first == "lines") {
                            seg = kv.second + (kv.first == "lines" ? " lines" : "");
                        } else if (kv.first == "+") {
                            seg = "+" + kv.second;
                        } else if (kv.first == "-") {
                            seg = "-" + kv.second;
                        } else if (kv.first == "exit") {
                            seg = "exit " + kv.second;
                        } else if (kv.first == "truncated" && kv.second == "true") {
                            seg = "truncated";
                        } else if (kv.first == "aborted" && kv.second == "true") {
                            seg = "aborted";
                        } else if (kv.first == "timeout" && kv.second == "true") {
                            seg = "timeout";
                        } else if (kv.first == "hint") {
                            seg = "hint:" + kv.second;
                        } else {
                            seg = kv.first + "=" + kv.second;
                        }
                        if (!metric_str.empty()) metric_str += " \xC2\xB7 "; // " · "
                        metric_str += seg;
                    }

                    const int summary_width = std::max(
                        20, chat_box.x_max - chat_box.x_min - 6);
                    std::string summary_line = renderable_tool_summary_line(
                        s, metric_str, summary_width);

                    Elements rows;
                    rows.push_back(hbox({
                        text("   <- ") | color(Color::GrayDark),
                        text(summary_line) | color(row_color) | flex,
                    }));

                    // Failed tool: render the first 3 lines of output dimmed
                    // below the summary so the error is visible without expand.
                    if (!is_success_summary(s) && !msg.content.empty()) {
                        int shown = 0;
                        size_t pos = 0;
                        while (pos < msg.content.size() && shown < 3) {
                            size_t nl = msg.content.find('\n', pos);
                            std::string line = (nl == std::string::npos)
                                ? msg.content.substr(pos)
                                : msg.content.substr(pos, nl - pos);
                            rows.push_back(hbox({
                                text("      ") | color(Color::GrayDark),
                                paragraph(line) | color(Color::GrayLight) | dim | flex,
                            }));
                            if (nl == std::string::npos) break;
                            pos = nl + 1;
                            ++shown;
                        }
                    }

                    auto block = vbox(std::move(rows));
                    if (focused_message) {
                        block = block | focus;
                    }
                    message_elements.push_back(block | focus_decorator | reflect(message_boxes[i]));
                } else {
                    // ---- Legacy fold path (also used when user pressed Ctrl+E
                    // to expand, and the 10-line cap acts as a secondary safety) ----
                    const int MAX_TOOL_LINES = 10;
                    std::string display_content = msg.content;
                    int line_count = 0;
                    for (char c : msg.content) if (c == '\n') line_count++;
                    if (msg.content.empty() || msg.content.back() != '\n') line_count++;

                    if (line_count > MAX_TOOL_LINES) {
                        size_t cut = 0;
                        int seen = 0;
                        while (cut < msg.content.size() && seen < MAX_TOOL_LINES) {
                            if (msg.content[cut] == '\n') seen++;
                            cut++;
                        }
                        display_content = msg.content.substr(0, cut);
                        if (!display_content.empty() && display_content.back() == '\n') {
                            display_content.pop_back();
                        }
                        int hidden = line_count - MAX_TOOL_LINES;
                        display_content += "\n... (" + std::to_string(hidden) + " more lines)";
                    }

                    auto line = hbox({
                        text("   <- ") | color(Color::GrayDark),
                        render_tool_result_lines_preserving_breaks(display_content) | flex,
                    });
                    if (focused_message) {
                        line = line | focus;
                    }
                    message_elements.push_back(line | focus_decorator | reflect(message_boxes[i]));
                }
            } else if (msg.role == "system") {
                auto line = hbox({
                    text(" i ") | bold | color(Color::Yellow),
                    paragraph(msg.content) | color(Color::Yellow) | flex,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator | reflect(message_boxes[i]));
            } else if (msg.role == "error") {
                auto line = hbox({
                    text(" ! ") | bold | color(Color::Red),
                    paragraph(msg.content) | color(Color::RedLight) | flex,
                });
                if (focused_message) {
                    line = line | focus;
                }
                message_elements.push_back(line | focus_decorator | reflect(message_boxes[i]));
            }
            message_elements.push_back(text(""));
        }

        // draggable-thick-scrollbar: thumb glyph identical to upstream
        // vscroll_indicator (┃╹╻),painted only in the rightmost reserved
        // column. We reserve 3 columns total so the *invisible* hit zone
        // is wider than 1 cell — the leftmost 2 reserved columns render
        // as whitespace but scrollbar_box.Contain() still matches them,
        // making mouse aim much easier without changing the visual rail
        // position. The decorator also enforces a minimum thumb height
        // (3 cells) so long sessions don't shrink the click target to a
        // single half-block.
        auto message_view = acecode::tui::thick_vscroll_bar(
                                vbox(std::move(message_elements)),
                                /*width=*/3,
                                scrollbar_box)
            | yframe | reflect(chat_box) | flex
            // mouse-selection-copy: visual feedback for drag-selection. The
            // decorator lives on the message_view so selection can span
            // multiple messages.
            | selectionBackgroundColor(Color::Blue)
            | selectionForegroundColor(Color::White);

        // -- Thinking indicator / tool progress --
        // Priority: if a tool is streaming output, show the live tool-progress
        // element instead of the thinking animation (the tool is the more
        // specific "in-progress" signal).
        Element thinking_element = text("");
        if (state.tool_running) {
            thinking_element = render_tool_progress(state);
        } else if (state.is_waiting) {
            int tick = anim_tick.load();
            int dot_count = (tick % 3) + 1; // 1, 2, 3 dots cycling

            std::string base = state.current_thinking_phrase;
            std::vector<std::string> utf8_chars;
            for (size_t i = 0; i < base.size();) {
                unsigned char c = base[i];
                size_t len = 1;
                if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                if (i + len > base.size()) len = base.size() - i;
                utf8_chars.push_back(base.substr(i, len));
                i += len;
            }

            int total_chars = static_cast<int>(utf8_chars.size());
            int wave_pos = tick % (total_chars > 0 ? total_chars + 2 : 8);

            Elements chars;
            for (int i = 0; i < total_chars; i++) {
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
                chars.push_back(text(utf8_chars[i]) | color(c));
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
        // Resume picker overlay above the prompt
        Element resume_picker_element = text("");
        if (state.resume_picker_active && !state.resume_items.empty()) {
            Elements picker_rows;
            picker_rows.push_back(
                text(" Resume a session (Up/Down/PgUp/PgDn/Home/End to navigate, Enter to confirm, Esc to cancel, 1-9 jump):")
                | bold | color(Color::Cyan));
            picker_rows.push_back(text(""));

            const int total = static_cast<int>(state.resume_items.size());
            const int visible = std::min(acecode::tui::kResumePickerVisibleRows, total);
            int offset = std::clamp(state.resume_view_offset, 0,
                                    std::max(0, total - visible));
            const int items_above = offset;
            const int items_below = std::max(0, total - offset - visible);

            // Top overflow indicator (always reserves a row to keep height stable).
            if (items_above > 0) {
                picker_rows.push_back(
                    text("  \xE2\x86\x91 " + std::to_string(items_above) + " more above")
                    | dim | color(Color::GrayLight));
            } else {
                picker_rows.push_back(text(""));
            }

            for (int i = offset; i < offset + visible; ++i) {
                bool selected = (i == state.resume_selected);
                auto row = text("  " + state.resume_items[i].display);
                if (selected) {
                    row = row | bold | color(Color::White) | bgcolor(Color::RGB(0, 80, 120));
                } else {
                    row = row | color(Color::GrayLight);
                }
                picker_rows.push_back(row);
            }

            // Bottom overflow indicator (also reserves a row).
            if (items_below > 0) {
                picker_rows.push_back(
                    text("  \xE2\x86\x93 " + std::to_string(items_below) + " more below")
                    | dim | color(Color::GrayLight));
            } else {
                picker_rows.push_back(text(""));
            }

            picker_rows.push_back(text(""));
            resume_picker_element = vbox(std::move(picker_rows)) | border | color(Color::Cyan);
        }
        Element rewind_picker_element = text("");
        if (state.rewind_picker_active && !state.rewind_items.empty()) {
            Elements picker_rows;
            if (state.rewind_mode_active) {
                const int selected =
                    std::clamp(state.rewind_selected, 0,
                               static_cast<int>(state.rewind_items.size()) - 1);
                const auto& item = state.rewind_items[selected];
                picker_rows.push_back(
                    text(" Rewind mode (Up/Down to select, Enter to confirm, Esc to go back, or type 1-9):")
                    | bold | color(Color::Cyan));
                picker_rows.push_back(text(" Target: " + item.preview) | color(Color::GrayLight));
                picker_rows.push_back(text(" Code rewind only covers ACECode file_edit/file_write changes; manual edits, shell commands, MCP tools, git operations, and external side effects are not tracked.")
                                      | color(Color::Yellow));
                picker_rows.push_back(text(""));
                for (int i = 0; i < static_cast<int>(state.rewind_modes.size()); ++i) {
                    bool selected_mode = (i == state.rewind_mode_selected);
                    const auto& mode = state.rewind_modes[i];
                    auto row = hbox({
                        text("  [" + std::to_string(i + 1) + "] " + mode.label + "  "),
                        text(mode.description) | color(Color::GrayLight),
                    });
                    if (selected_mode) {
                        row = row | bold | color(Color::White) | bgcolor(Color::RGB(0, 80, 120));
                    } else {
                        row = row | color(Color::GrayLight);
                    }
                    picker_rows.push_back(row);
                }
            } else {
                picker_rows.push_back(
                    text(" Rewind to a user turn (Up/Down/PgUp/PgDn/Home/End to navigate, Enter to confirm, Esc to cancel, 1-9 jump):")
                    | bold | color(Color::Cyan));
                picker_rows.push_back(text(""));

                const int total = static_cast<int>(state.rewind_items.size());
                const int visible = std::min(acecode::tui::kRewindPickerVisibleRows, total);
                int offset = std::clamp(state.rewind_view_offset, 0,
                                        std::max(0, total - visible));
                const int items_above = offset;
                const int items_below = std::max(0, total - offset - visible);

                if (items_above > 0) {
                    picker_rows.push_back(
                        text("  \xE2\x86\x91 " + std::to_string(items_above) + " more above")
                        | dim | color(Color::GrayLight));
                } else {
                    picker_rows.push_back(text(""));
                }

                for (int i = offset; i < offset + visible; ++i) {
                    bool selected = (i == state.rewind_selected);
                    auto row = text("  " + state.rewind_items[i].display);
                    if (selected) {
                        row = row | bold | color(Color::White) | bgcolor(Color::RGB(0, 80, 120));
                    } else {
                        row = row | color(Color::GrayLight);
                    }
                    picker_rows.push_back(row);
                }

                if (items_below > 0) {
                    picker_rows.push_back(
                        text("  \xE2\x86\x93 " + std::to_string(items_below) + " more below")
                        | dim | color(Color::GrayLight));
                } else {
                    picker_rows.push_back(text(""));
                }
            }
            picker_rows.push_back(text(""));
            rewind_picker_element = vbox(std::move(picker_rows)) | border | color(Color::Cyan);
        }
        Element slash_dropdown_element = render_slash_dropdown(state);

        // AskUserQuestion overlay —— 和 confirm_pending 互斥,在渲染层面
        // 显式让 ask 优先(事件层在 confirm 分支之前也已经拦截,这里只是
        // 作为显式护栏)。
        Element ask_overlay_element = text("");
        if (state.ask_pending && !state.ask_questions.empty() &&
            state.ask_current_question >= 0 &&
            state.ask_current_question <
                static_cast<int>(state.ask_questions.size())) {
            const auto& q = state.ask_questions[state.ask_current_question];
            Elements rows;
            std::string header_line = " Question " +
                std::to_string(state.ask_current_question + 1) + "/" +
                std::to_string(state.ask_questions.size()) +
                "  [" + q.header + "]";
            rows.push_back(text(header_line) | bold | color(Color::Cyan));
            rows.push_back(text(" " + q.question) | color(Color::White));
            rows.push_back(text(""));

            int option_count = static_cast<int>(q.options.size());
            int total_rows = option_count + 1; // +1 for "Other..."
            for (int i = 0; i < total_rows; ++i) {
                bool is_other = (i == option_count);
                bool focused = (i == state.ask_option_focus);
                std::string marker;
                if (q.multi_select) {
                    bool checked = !is_other && i < static_cast<int>(state.ask_multi_selected.size()) &&
                                   state.ask_multi_selected[i];
                    marker = checked ? "[x] " : "[ ] ";
                } else {
                    // 单选时焦点位置用实心圆点;其它位置留空圆点。
                    marker = focused ? "(\xE2\x97\x8F) " : "( ) ";
                }
                std::string prefix = focused ? " \xE2\x96\xB8 " : "   ";
                std::string body;
                if (is_other) {
                    body = "Other...";
                } else {
                    body = q.options[i].label;
                    if (!q.options[i].description.empty()) {
                        body += "  ";
                        body += q.options[i].description;
                    }
                }
                auto row = text(prefix + marker + body);
                if (focused) {
                    row = row | bold | color(Color::White) | bgcolor(Color::RGB(0, 60, 100));
                } else {
                    row = row | color(Color::GrayLight);
                }
                rows.push_back(row);
            }
            rows.push_back(text(""));

            std::string hint = q.multi_select
                ? " \xE2\x86\x91\xE2\x86\x93 move   Space toggle   Enter submit   Esc cancel"
                : " \xE2\x86\x91\xE2\x86\x93 move   Enter select   Esc cancel";
            rows.push_back(text(hint) | dim | color(Color::GrayDark));

            if (state.ask_other_input_active) {
                rows.push_back(text(""));
                rows.push_back(text(" Custom answer (Enter to submit, Esc to back out):")
                               | color(Color::Yellow));
            }
            ask_overlay_element = vbox(std::move(rows)) | border | color(Color::Cyan);
        }

        // Tool confirmation overlay —— 取代旧的单行 "y/a/n" 提示。
        // 三个固定选项:
        //   0  Yes
        //   1  Yes, allow all edits during this session (shift+tab)
        //   2  No
        // ↑↓ 移焦点,Enter 提交焦点项,1/2/3 数字键直选,Shift+Tab 直接选第二项,
        // Esc → Deny。事件分支在 CatchEvent 中,渲染层只是把状态画出来。
        Element confirm_overlay_element = text("");
        if (state.confirm_pending) {
            Elements rows;
            std::string title = acecode::tui::build_confirm_question(
                state.confirm_tool_name, state.confirm_tool_args);
            // build_confirm_question 可能返回多行(bash 把 command 附在第二行),
            // 按 \n 拆开逐行 push,首行加粗。
            bool first = true;
            size_t pos = 0;
            while (pos <= title.size()) {
                size_t nl = title.find('\n', pos);
                std::string line = (nl == std::string::npos)
                    ? title.substr(pos)
                    : title.substr(pos, nl - pos);
                if (first) {
                    rows.push_back(text(" " + line) | bold | color(Color::Yellow));
                    first = false;
                } else {
                    rows.push_back(text(line) | color(Color::GrayLight));
                }
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
            rows.push_back(text(""));

            const std::array<std::string, 3> labels = {
                "1. Yes",
                "2. Yes, allow all edits during this session (shift+tab)",
                "3. No",
            };
            const int focus = std::clamp(state.confirm_focus, 0, 2);
            for (int i = 0; i < 3; ++i) {
                bool focused = (i == focus);
                std::string prefix = focused ? " \xE2\x9D\xAF " : "   ";
                auto row = text(prefix + labels[i]);
                if (focused) {
                    row = row | bold | color(Color::White) | bgcolor(Color::RGB(0, 60, 100));
                } else {
                    row = row | color(Color::GrayLight);
                }
                rows.push_back(row);
            }
            rows.push_back(text(""));
            rows.push_back(
                text(" \xE2\x86\x91\xE2\x86\x93 move   Enter select   1/2/3 jump   Esc deny")
                | dim | color(Color::GrayDark));
            confirm_overlay_element = vbox(std::move(rows)) | border | color(Color::Yellow);
        }

        if (state.exit_confirm_pending) {
            // 优先级最高:Ctrl+C 退出确认 overlay 可在任何其他状态(ask /
            // confirm / 普通输入)上叠加显示。事件拦截分支也放在最早。
            prompt_line = hbox({
                text(" Exit acecode? ") | bold | color(Color::Yellow),
                text("y") | bold | color(Color::Green),
                text(" / ") | color(Color::YellowLight),
                text("N") | bold | color(Color::Red),
                text(" (any other key cancels)") | dim | color(Color::GrayDark),
            });
        } else if (state.ask_pending) {
            // ask active 时:把输入框留给 Other 自定义文本态使用;其它状态下
            // 显示静态提示,吞掉字符输入(CatchEvent 不透传非导航键)。
            if (state.ask_other_input_active) {
                prompt_line = hbox({
                    text(" ? ") | bold | color(Color::Yellow),
                    input_with_esc->Render() | flex,
                });
            } else {
                prompt_line = hbox({
                    text(" ? answering: ") | bold | color(Color::Yellow),
                    text("use arrows + Enter (Esc to cancel)") | dim | color(Color::GrayDark),
                });
            }
        } else if (state.confirm_pending) {
            // overlay 已经把选项画在 message_view 之上,prompt_line 仅作静态
            // 提示并吞掉字符输入(CatchEvent 中 confirm overlay handler 拦截非
            // 导航键,这里的 hbox 不渲染 input_with_esc 是为了让光标不在输入
            // 框里闪、误导用户去打字)。
            prompt_line = hbox({
                text(" [" + state.confirm_tool_name + "] ") | bold | color(Color::Magenta),
                text("awaiting confirmation \xE2\x80\x94 use \xE2\x86\x91\xE2\x86\x93 + Enter (Esc to deny)")
                    | dim | color(Color::GrayDark),
            });
        } else {
            Elements prompt_parts;
            if (state.input_mode == InputMode::Shell) {
                prompt_parts.push_back(text(" ! ") | bold | color(Color::Red));
            } else {
                prompt_parts.push_back(text(" > ") | bold | color(Color::Cyan));
            }
            prompt_parts.push_back(input_with_esc->Render() | flex);
            if (!state.pending_queue.empty()) {
                prompt_parts.push_back(
                    text(" [" + std::to_string(state.pending_queue.size()) + " queued]") | dim | color(Color::GrayDark));
            }
            prompt_line = hbox(std::move(prompt_parts));
        }

        // -- Bottom status bar --
        std::string perm_mode_str = std::string("mode: ") + PermissionManager::mode_name(permissions.mode());
        Element token_el = state.token_status.empty()
            ? text("")
            : text("  " + state.token_status + "  ") | dim | color(Color::CyanLight);
        // Tool timer chip — persistent even when the main progress element is
        // obscured by overlays or scrolled out of view. Thinking-timer chip
        // shows elapsed time + live output-token estimate while the agent is
        // waiting on the LLM. The two are mutually exclusive (thinking chip
        // returns empty when tool_running is true), so they occupy the same
        // slot in the bottom bar.
        Element tool_timer_el = render_tool_timer_chip(state);
        Element thinking_timer_el = render_thinking_timer_chip(state);
        Element bottom_bar;
        if (dangerous_mode) {
            bottom_bar = hbox({
                text("  [DANGEROUS MODE]") | bold | color(Color::Red),
                filler(),
                thinking_timer_el,
                tool_timer_el,
                token_el,
                text(perm_mode_str + "  ") | dim | color(Color::GrayDark),
            });
        } else if (state.is_waiting || state.tool_running) {
            bottom_bar = hbox({
                text("  esc / ctrl+c to interrupt") | dim | color(Color::GrayDark),
                filler(),
                thinking_timer_el,
                tool_timer_el,
                token_el,
                text(perm_mode_str + "  ") | dim | color(Color::GrayDark),
            });
        } else {
            bottom_bar = hbox({
                text("  ctrl+p: cycle permission mode") | dim | color(Color::GrayDark),
                filler(),
                tool_timer_el,
                token_el,
                text(perm_mode_str + "  ") | dim | color(Color::GrayDark),
            });
        }

        // IME composition window positioning is handled by FTXUI's cursor
        // system (focusCursorBlock) which emits ANSI sequences to place the
        // terminal cursor at the caret. Windows Terminal/ConPTY uses this
        // to position the IME window. The Win32 IME APIs (ImmSetComposition
        // Window) don't work under ConPTY.

        Color outer_border_color = (state.input_mode == InputMode::Shell)
            ? Color::Red
            : Color::GrayLight;

        return vbox({
            header,
            separatorHeavy() | color(Color::GrayDark),
            message_view,
            resume_picker_element,
            rewind_picker_element,
            ask_overlay_element,
            confirm_overlay_element,
            slash_dropdown_element,
            thinking_element,
            separatorLight() | color(Color::GrayDark),
            prompt_line,
            bottom_bar,
        }) | borderRounded | color(outer_border_color);
    });

#ifdef _WIN32
    // FTXUI applies its console mode inside Loop()/PreMain, so queue this
    // adjustment as the first loop task. From then on Ctrl+C arrives as
    // Event::CtrlC instead of being consumed by the Windows console layer.
    screen.Post(prepare_windows_ctrl_c_handling_after_ftxui_install);
#endif
    // Enable bracketed paste so the terminal frames pasted text with
    // ESC[200~ … ESC[201~. FTXUI exposes those markers as Event::Special, and
    // the paste accumulator in the CatchEvent body intercepts them before
    // normal Return / character handlers run — preventing pasted newlines
    // from accidentally submitting partial prompts. The escape sequence is
    // a private mode setting; it's harmless on terminals that don't support
    // it (they ignore unknown DECSET codes).
    std::cout << acecode::tui::kBracketedPasteEnableSeq << std::flush;
    screen.Loop(renderer);
    // Restore the terminal so subsequent shell prompts on the same session
    // don't see ESC[200~/201~ wrappers around future pastes.
    std::cout << acecode::tui::kBracketedPasteDisableSeq << std::flush;

    // Stop the ctrl handler from poking at `screen` once Loop returns; the
    // ScreenInteractive will be destroyed at end of scope.
    g_active_screen.store(nullptr, std::memory_order_release);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
#endif

    running = false;

    // Graceful shutdown: abort agent, unblock confirm_cv / ask_cv, then join worker
    agent_aborting = true;
    agent_loop.abort();
    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.confirm_pending) {
            state.confirm_pending = false;
            state.confirm_result = PermissionResult::Deny;
            state.confirm_cv.notify_one();
        }
        if (state.ask_pending) {
            // AskUserQuestion 工具线程也在 wait,通知它醒来走拒绝分支。
            // agent_loop.abort() 已经置 abort_flag,工具的 wait 谓词因此成立。
            state.ask_pending = false;
            state.ask_result_ok = false;
            state.ask_cv.notify_one();
        }
    }
    agent_loop.shutdown();

    // Tear down MCP child processes after the agent worker has stopped, so no
    // in-flight tool calls can race with the clients being destroyed.
    mcp_manager.shutdown();

    // Abort and join any in-progress compact thread
    state.compact_abort_requested.store(true);
    if (state.compact_thread.joinable()) {
        state.compact_thread.join();
    }

    if (anim_thread.joinable()) {
        anim_thread.join();
    }

    if (auth_thread.joinable()) {
        auth_thread.join();
    }

    // Finalize session before exit
    session_manager.finalize();
    session_manager.cleanup_old_sessions(config.max_sessions);

    // Print session ID so user knows how to resume
    auto exit_sid = session_manager.current_session_id();
    g_session_manager = nullptr;
    if (!exit_sid.empty()) {
        std::cerr << "\nacecode: session " << exit_sid
                  << " saved. Resume with: acecode --resume " << exit_sid << std::endl;
    }

    return 0;
}
