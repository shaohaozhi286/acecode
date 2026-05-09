// acetui/src/terminal_win.cpp — Win32 实现。
//
// 进程内用一个 anonymous-namespace static 状态保存原 console mode,
// 这样 enable_raw / restore / size / read_event / write 都可以是
// Terminal 的静态成员函数。Terminal 不持有任何实例,头文件零状态。

#include "acetui/terminal.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace acetui {

namespace {

HANDLE g_in           = INVALID_HANDLE_VALUE;
HANDLE g_out          = INVALID_HANDLE_VALUE;
DWORD  g_orig_in_mode = 0;
DWORD  g_orig_out_mode= 0;
UINT   g_orig_in_cp   = 0;
UINT   g_orig_out_cp  = 0;
bool   g_modes_saved  = false;
Size   g_last_size{80, 24};

}  // namespace

bool Terminal::enable_raw() {
    if (g_modes_saved) {
        return true;  // 二次调用 no-op
    }

    g_in  = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!GetConsoleMode(g_in, &g_orig_in_mode))   return false;
    if (!GetConsoleMode(g_out, &g_orig_out_mode)) return false;
    g_orig_in_cp  = GetConsoleCP();
    g_orig_out_cp = GetConsoleOutputCP();
    g_modes_saved = true;

    // stdin:只保留 WINDOW_BUFFER_SIZE 事件;关掉 LINE_INPUT / ECHO_INPUT /
    // PROCESSED_INPUT。PROCESSED_INPUT 关掉后 Ctrl+C 不再触发默认 SIGINT,
    // 我们自己在 App 层翻译 Ctrl+C 为退出。
    DWORD in_mode = ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode(g_in, in_mode)) {
        g_modes_saved = false;
        return false;
    }

    // stdout:VT processing 必装,acetui 整体依赖 ANSI escape。
    DWORD out_mode = ENABLE_PROCESSED_OUTPUT
                   | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                   | ENABLE_WRAP_AT_EOL_OUTPUT;
    if (!SetConsoleMode(g_out, out_mode)) {
        SetConsoleMode(g_in, g_orig_in_mode);
        g_modes_saved = false;
        return false;
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    return true;
}

void Terminal::restore() {
    if (!g_modes_saved) return;
    SetConsoleMode(g_in,  g_orig_in_mode);
    SetConsoleMode(g_out, g_orig_out_mode);
    SetConsoleCP(g_orig_in_cp);
    SetConsoleOutputCP(g_orig_out_cp);
    g_modes_saved = false;
}

Size Terminal::size() {
    if (g_out == INVALID_HANDLE_VALUE) {
        return g_last_size;
    }
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(g_out, &csbi)) {
        return g_last_size;
    }
    Size s;
    s.width  = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
    s.height = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    if (s.width  <= 0) s.width  = 80;
    if (s.height <= 0) s.height = 24;
    g_last_size = s;
    return s;
}

void Terminal::write(std::string_view bytes) {
    if (g_out == INVALID_HANDLE_VALUE || bytes.empty()) return;
    const char* p = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        DWORD chunk = remaining > 0xFFFFu
                          ? 0xFFFFu
                          : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (!WriteFile(g_out, p, chunk, &written, nullptr) || written == 0) {
            return;  // 写失败放弃,不抛
        }
        p         += written;
        remaining -= written;
    }
}

namespace {

// 把 Win32 KEY_EVENT_RECORD 翻译成 acetui::KeyEvent。codepoint 为 0
// 表示 "phase 1 用不上的 key,跳过"。
bool translate_key_event(const KEY_EVENT_RECORD& k, KeyEvent& out) {
    if (!k.bKeyDown) return false;

    Modifier mods = Modifier::None;
    if (k.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        mods = mods | Modifier::Ctrl;
    }
    if (k.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) {
        mods = mods | Modifier::Alt;
    }
    if (k.dwControlKeyState & SHIFT_PRESSED) {
        mods = mods | Modifier::Shift;
    }

    char32_t cp = 0;
    switch (k.wVirtualKeyCode) {
        case VK_BACK:   cp = key::kBackspace; break;
        case VK_RETURN: cp = key::kEnter;     break;
        case VK_ESCAPE: cp = key::kEsc;       break;
        case VK_TAB:    cp = 0x09;            break;
        case VK_LEFT:   cp = key::kLeft;      break;
        case VK_RIGHT:  cp = key::kRight;     break;
        case VK_UP:     cp = key::kUp;        break;
        case VK_DOWN:   cp = key::kDown;      break;
        case VK_HOME:   cp = key::kHome;      break;
        case VK_END:    cp = key::kEnd;       break;
        case VK_DELETE: cp = key::kDelete;    break;
        default: {
            // Ctrl+letter:从 vk 直接取字母,避免 uChar 是 control code
            // (Ctrl+A → uChar 0x01)无法直接识别。
            if (has_mod(mods, Modifier::Ctrl) &&
                k.wVirtualKeyCode >= 'A' && k.wVirtualKeyCode <= 'Z') {
                cp = static_cast<char32_t>(k.wVirtualKeyCode);
            } else if (k.uChar.UnicodeChar >= 0x20 &&
                       k.uChar.UnicodeChar != 0x7F) {
                // BMP 单 wchar_t;surrogate pair 不在 phase 1 范围。
                cp = static_cast<char32_t>(k.uChar.UnicodeChar);
            }
            break;
        }
    }

    if (cp == 0) return false;
    out.codepoint = cp;
    out.mods      = mods;
    return true;
}

}  // namespace

std::optional<Event> Terminal::read_event(
    std::optional<std::chrono::milliseconds> timeout) {
    if (g_in == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    while (true) {
        if (timeout.has_value()) {
            DWORD ms = static_cast<DWORD>(
                timeout->count() < 0 ? 0 : timeout->count());
            DWORD r = WaitForSingleObject(g_in, ms);
            if (r == WAIT_TIMEOUT) {
                return std::nullopt;
            }
            if (r != WAIT_OBJECT_0) {
                return std::nullopt;
            }
        }
        // 不传 timeout 时 ReadConsoleInputW 自身阻塞。

        INPUT_RECORD rec;
        DWORD nread = 0;
        if (!ReadConsoleInputW(g_in, &rec, 1, &nread) || nread == 0) {
            continue;
        }

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            Size sz = Terminal::size();
            return Event{ResizeEvent{sz.width, sz.height}};
        }

        if (rec.EventType == KEY_EVENT) {
            KeyEvent ke;
            if (translate_key_event(rec.Event.KeyEvent, ke)) {
                return Event{ke};
            }
            // 未识别 key:继续 loop 等下一事件。
            continue;
        }

        // MOUSE_EVENT / FOCUS_EVENT / MENU_EVENT — phase 1 不处理。
    }
}

}  // namespace acetui
