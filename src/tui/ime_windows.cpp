// Windows IME composition window positioning. Win32-only.
// Extracted from main.cpp.
#include "tui/ime_windows.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#pragma comment(lib, "Imm32.lib")

#include <string>
#include <sstream>
#include <ftxui/screen/string.hpp>
#include "utils/logger.hpp"

namespace acecode { namespace tui {

static int max_int(int a, int b) { return a > b ? a : b; }

static int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
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
              ", last_error=" + dword_to_hex(GetLastError()) + ", using foreground window");
    return target;
}

void update_ime_composition_window(const std::string& input_text,
                                   bool show_bottom_bar,
                                   bool confirm_pending,
                                   const std::string& confirm_tool_name) {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) { LOG_WARN("IME: GetConsoleWindow returned null"); return; }

    HANDLE hconsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hconsole == INVALID_HANDLE_VALUE) {
        LOG_WARN("IME: GetStdHandle failed, last_error=" + dword_to_hex(GetLastError()));
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
        LOG_WARN("IME: invalid visible size"); return;
    }

    int cell_width = 0, cell_height = 0;
    CONSOLE_FONT_INFOEX cfi{};
    cfi.cbSize = sizeof(cfi);
    if (GetCurrentConsoleFontEx(hconsole, FALSE, &cfi)) {
        cell_width = max_int(1, static_cast<int>(cfi.dwFontSize.X));
        cell_height = max_int(1, static_cast<int>(cfi.dwFontSize.Y));
    } else {
        RECT client{};
        if (!GetClientRect(hwnd, &client)) { LOG_WARN("IME: both font/client failed"); return; }
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        if (cw <= 0 || ch <= 0) { LOG_WARN("IME: invalid client size"); return; }
        cell_width = max_int(1, cw / visible_cols);
        cell_height = max_int(1, ch / visible_rows);
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
        ImmSetCompositionWindow(himc, &composition);
        ImmSetCandidateWindow(himc, &candidate);
        ImmReleaseContext(ime_target, himc);
        return;
    }

    LOG_WARN("IME: ImmGetContext returned null, target=" + ptr_to_hex(ime_target));
    HWND default_ime_window = ImmGetDefaultIMEWnd(ime_target);
    if (!default_ime_window) { LOG_WARN("IME: ImmGetDefaultIMEWnd returned null"); return; }
    SendMessage(default_ime_window, WM_IME_CONTROL, IMC_SETCOMPOSITIONWINDOW, reinterpret_cast<LPARAM>(&composition));
    SendMessage(default_ime_window, WM_IME_CONTROL, IMC_SETCANDIDATEPOS, reinterpret_cast<LPARAM>(&candidate));
}

}} // namespace acecode::tui

#endif // _WIN32
