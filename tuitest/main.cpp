// tuitest — Win32 raw-mode + DECSTBM scroll-region 底部输入条 demo。
//
// 模仿 codex 的 codex-rs/tui/src/insert_history.rs::insert_history_lines_with_mode_and_wrap_policy
// 的 "Standard" 路径:viewport(widget,即贴底的输入条)永远停在屏幕最底
// 3 行,Enter 提交时通过 DECSTBM 滚动区让 viewport 上方那一片向上滚一行,
// 在底部腾出位置写新历史。widget 三行字符不被任何 escape 序列覆盖,因此
// 没有 erase + redraw 抖动,鼠标滚轮 / 选中 / 复制全走终端原生通道。
//
// 提交流程的 5 步 escape:
//   1. \x1b[1;<viewport_top - 1>r       设置滚动区 = [1 .. viewport 上方]
//   2. \x1b[<viewport_top - 1>;1H        cursor 落到滚动区底
//   3. \r\n + 历史行                     滚动区上滚 1,顶端进 scrollback
//   4. \x1b[r                            重置滚动区
//   5. \x1b[<input_row>;<input_col>H     cursor 回输入行原位
//
// 普通字符 / Backspace 只重画输入行单行(绝对定位 + EraseInLine),不动
// 两条 separator。
//
// Limitations(demo 阶段不处理):
//   - 方向键 / Home / End / 行内编辑(只支持追加 + Backspace)
//   - 输入 buffer 超出终端宽度时仅 truncate 显示(buf 仍存全),不做 wrap-aware widget
//   - CJK 显示宽度按 1 列估算(WideCharToMultiByte 后简单按 byte 算 col)
//   - Windows-only;POSIX 端等后续

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Console handles + saved modes
// ---------------------------------------------------------------------------

HANDLE g_in  = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
DWORD  g_orig_in_mode  = 0;
DWORD  g_orig_out_mode = 0;
UINT   g_orig_in_cp    = 0;
UINT   g_orig_out_cp   = 0;
bool   g_modes_saved   = false;

// ---------------------------------------------------------------------------
// Viewport state(widget 高度 = 3:上 separator + 输入行 + 下 separator)
// ---------------------------------------------------------------------------

constexpr int kViewportHeight = 3;
int g_viewport_top  = 0;   // 1-based,widget 上 separator 所在行
int g_screen_width  = 80;
int g_screen_height = 24;

// 给定屏幕高度,算 widget 上 separator 应该贴底放在哪一行(1-based)。
int compute_viewport_top(int screen_height) {
    int top = screen_height - kViewportHeight + 1;
    return top < 1 ? 1 : top;
}

// ---------------------------------------------------------------------------
// Console helpers
// ---------------------------------------------------------------------------

// enable_raw — 关 LINE_INPUT/ECHO/PROCESSED_INPUT,开 VT processing。
// 失败返回 false:caller 必须报错并退出。
bool enable_raw() {
    g_in  = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (!GetConsoleMode(g_in,  &g_orig_in_mode))  return false;
    if (!GetConsoleMode(g_out, &g_orig_out_mode)) return false;
    g_orig_in_cp  = GetConsoleCP();
    g_orig_out_cp = GetConsoleOutputCP();
    g_modes_saved = true;

    // 输入端:保留 WINDOW_BUFFER_SIZE 事件;不开 PROCESSED_INPUT,Ctrl+C 我们自己处理。
    DWORD in_mode = ENABLE_WINDOW_INPUT;
    if (!SetConsoleMode(g_in, in_mode)) {
        return false;
    }

    // 输出端:必须开 VT processing,demo 整体依赖 DECSTBM。
    DWORD out_mode = ENABLE_PROCESSED_OUTPUT
                   | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                   | ENABLE_WRAP_AT_EOL_OUTPUT;
    if (!SetConsoleMode(g_out, out_mode)) {
        // 失败 — 立刻把 in mode 也复原,避免半改状态。
        SetConsoleMode(g_in, g_orig_in_mode);
        return false;
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    return true;
}

void restore() {
    if (!g_modes_saved) return;
    SetConsoleMode(g_in,  g_orig_in_mode);
    SetConsoleMode(g_out, g_orig_out_mode);
    SetConsoleCP(g_orig_in_cp);
    SetConsoleOutputCP(g_orig_out_cp);
}

// 当前可视窗口宽 × 高(不是 buffer 总尺寸)。失败返回 80×24。
void term_size(int& w, int& h) {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (!GetConsoleScreenBufferInfo(g_out, &csbi)) {
        w = 80; h = 24; return;
    }
    w = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
    h = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    if (w <= 0) w = 80;
    if (h <= 0) h = 24;
}

void out(const std::string& s) {
    DWORD written = 0;
    WriteFile(g_out, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
}

// ---------------------------------------------------------------------------
// ANSI escape helpers — 集中收口,避免散落 magic string
// ---------------------------------------------------------------------------

inline std::string move_to(int row /*1-based*/, int col /*1-based*/) {
    return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}
inline std::string set_scroll_region(int top /*1-based*/, int bottom /*1-based*/) {
    return "\033[" + std::to_string(top) + ";" + std::to_string(bottom) + "r";
}
inline std::string reset_scroll_region() {
    return "\033[r";
}
inline std::string erase_in_line() {
    return "\033[K";
}
inline std::string erase_to_screen_end() {
    return "\033[J";
}

// ---------------------------------------------------------------------------
// UTF-8 utilities
// ---------------------------------------------------------------------------

void utf8_pop_codepoint(std::string& s) {
    while (!s.empty() &&
           (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
        s.pop_back();
    }
    if (!s.empty()) {
        s.pop_back();
    }
}

// ---------------------------------------------------------------------------
// Widget rendering
// ---------------------------------------------------------------------------

// 写一整行 separator(`-` 重复 g_screen_width 次)。
void draw_separator(int row) {
    std::string sep(static_cast<size_t>(g_screen_width), '-');
    std::string b;
    b += move_to(row, 1);
    b += erase_in_line();
    b += sep;
    out(b);
}

// 写输入行 "> <buf>",cursor 留在 buf 末尾。
void draw_input_line(int row, const std::string& buf) {
    std::string b;
    b += move_to(row, 1);
    b += erase_in_line();
    b += "> ";
    b += buf;
    out(b);
}

// 仅重画输入行(separator 不动)— 字符按键 / Backspace 走这里。
void redraw_input_line(const std::string& buf) {
    draw_input_line(g_viewport_top + 1, buf);
}

// 整体重画 widget(启动 / resize 用)。
void draw_full_widget(const std::string& buf) {
    draw_separator(g_viewport_top);
    draw_input_line(g_viewport_top + 1, buf);
    draw_separator(g_viewport_top + 2);
    redraw_input_line(buf);  // 把 cursor 拉回输入行末尾
}

// 启动:发 kViewportHeight 个 \r\n 撞屏底,腾出 viewport 空间;然后画 widget。
void bootstrap_viewport() {
    term_size(g_screen_width, g_screen_height);
    g_viewport_top = compute_viewport_top(g_screen_height);

    for (int i = 0; i < kViewportHeight; ++i) {
        out("\r\n");
    }
    draw_full_widget(std::string{});
}

// 提交 buf 进 scrollback:DECSTBM 滚动区把 viewport 上方那一片上滚 1 行,
// 在腾出的位置写 line。widget 三行不被覆盖。
//
// input_col 是 cursor 提交后该回到的输入行列(buf 已清空 = 3 = "> " 之后)。
void commit_history_line(const std::string& line, int input_col) {
    if (g_viewport_top <= 1) {
        // widget 占满整屏,没有上方空间可滚 — demo 不处理这种极小窗口。
        return;
    }
    std::string b;
    // 1) 滚动区 = [1 .. viewport_top - 1](widget 上方那一片)
    b += set_scroll_region(1, g_viewport_top - 1);
    // 2) cursor 落到滚动区底端那行
    b += move_to(g_viewport_top - 1, 1);
    // 3) \r\n 触发滚动区上滚 1 行,在底部腾出位置;接着写 line
    b += "\r\n";
    b += line;
    // 4) 重置滚动区(全屏)
    b += reset_scroll_region();
    // 5) cursor 回输入行
    b += move_to(g_viewport_top + 1, input_col);
    out(b);
}

// 退出:清 widget + cursor 留在 widget 顶,shell prompt 自然在那行接续。
void exit_cleanly() {
    std::string b;
    b += move_to(g_viewport_top, 1);
    b += erase_to_screen_end();
    b += move_to(g_viewport_top, 1);
    out(b);
}

}  // namespace

int main() {
    if (!enable_raw()) {
        std::fprintf(stderr,
            "tuitest: 无法启用 ENABLE_VIRTUAL_TERMINAL_PROCESSING,当前终端不支持 "
            "DECSTBM/VT,demo 无法启动。\n");
        if (g_modes_saved) {
            restore();
        }
        return 1;
    }

    bootstrap_viewport();

    std::string buf;
    bool running = true;

    while (running) {
        INPUT_RECORD rec;
        DWORD nread = 0;
        if (!ReadConsoleInputW(g_in, &rec, 1, &nread) || nread == 0) {
            continue;
        }

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            // resize:重算 viewport,按新尺寸重画 widget。
            // 旧 widget 字符可能因 terminal reflow 仍残留在屏幕上方,后续
            // commit 触发滚动区上滚时会被自动处理。
            term_size(g_screen_width, g_screen_height);
            g_viewport_top = compute_viewport_top(g_screen_height);

            std::string b;
            b += move_to(g_viewport_top, 1);
            b += erase_to_screen_end();
            out(b);

            draw_full_widget(buf);
            continue;
        }

        if (rec.EventType != KEY_EVENT) {
            continue;
        }
        const KEY_EVENT_RECORD& k = rec.Event.KeyEvent;
        if (!k.bKeyDown) {
            continue;
        }

        const WCHAR wc   = k.uChar.UnicodeChar;
        const WORD  vk   = k.wVirtualKeyCode;
        const DWORD ctrl = k.dwControlKeyState;
        const bool is_ctrl_c =
            (vk == 'C') && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED));

        if (vk == VK_ESCAPE || is_ctrl_c) {
            running = false;
            break;
        }

        if (vk == VK_RETURN) {
            // 提交一行 → scrollback;清 buf;重画输入行。
            commit_history_line(buf, /*input_col=*/3);
            buf.clear();
            redraw_input_line(buf);
            continue;
        }

        if (vk == VK_BACK) {
            if (!buf.empty()) {
                utf8_pop_codepoint(buf);
                redraw_input_line(buf);
            }
            continue;
        }

        if (wc >= 0x20) {
            // UTF-16(BMP)→ UTF-8。surrogate pair 暂不处理。
            char utf8[8] = {0};
            const int n = WideCharToMultiByte(
                CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8), nullptr, nullptr);
            if (n > 0) {
                buf.append(utf8, static_cast<size_t>(n));
                redraw_input_line(buf);
            }
            continue;
        }
    }

    exit_cleanly();
    restore();
    return 0;
}
