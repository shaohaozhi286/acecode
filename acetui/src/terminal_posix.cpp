// acetui/src/terminal_posix.cpp — POSIX (Linux/macOS) 实现。
//
// 在 Win32 开发机上本文件不参与编译(cmake 用 if(WIN32) 排除)。POSIX 端
// 的实测延后到能上 Linux/macOS CI 时一起做。本期目标是:在装好 termios
// 头的环境上必须 0 错误编译,API 形态与 Win32 端完全一致。
//
// 用了 termios + read + ioctl(TIOCGWINSZ) + select(2) 做 timeout 阻塞
// 拉事件。Esc 单按键与 escape sequence 用 50ms 延迟区分(收到 0x1B
// 后再 select 50ms;无后续字节 → 单 Esc;有后续 → phase 1 暂时丢弃)。
// SIGWINCH 走 sig_atomic_t flag,在 read_event 进入时检查并优先返回
// ResizeEvent。

#include "acetui/terminal.hpp"

#include <csignal>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace acetui {

namespace {

termios g_orig_termios{};
bool    g_termios_saved = false;
Size    g_last_size{80, 24};

volatile std::sig_atomic_t g_winch_flag = 0;

void sigwinch_handler(int /*sig*/) {
    g_winch_flag = 1;
}

}  // namespace

bool Terminal::enable_raw() {
    if (g_termios_saved) return true;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) {
        return false;
    }

    termios raw = g_orig_termios;
    cfmakeraw(&raw);
    // 我们仍然希望读到 \r 而不是 \n,后续解析层统一处理。cfmakeraw 已经
    // 关掉 ICRNL/INLCR/IGNCR/ICANON/ECHO,够 phase 1 用。
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return false;
    }

    g_termios_saved = true;

    struct sigaction sa{};
    sa.sa_handler = sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, nullptr);

    return true;
}

void Terminal::restore() {
    if (!g_termios_saved) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    g_termios_saved = false;
}

Size Terminal::size() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
        return g_last_size;
    }
    Size s;
    s.width  = ws.ws_col > 0 ? ws.ws_col : 80;
    s.height = ws.ws_row > 0 ? ws.ws_row : 24;
    g_last_size = s;
    return s;
}

void Terminal::write(std::string_view bytes) {
    const char* p     = bytes.data();
    size_t remaining  = bytes.size();
    while (remaining > 0) {
        ssize_t n = ::write(STDOUT_FILENO, p, remaining);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        p         += n;
        remaining -= static_cast<size_t>(n);
    }
}

namespace {

// 等待 stdin 有可读字节,带 timeout(ms,-1 = 阻塞)。返回 true = 有字节
// 可读;false = 超时或被信号打断。
bool wait_readable(int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{};
    timeval* tvp = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }
    int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, tvp);
    return r > 0 && FD_ISSET(STDIN_FILENO, &fds);
}

// 读 1 字节;返回 -1 表示超时 / 无字节。
int read_byte_with_timeout(int timeout_ms) {
    if (!wait_readable(timeout_ms)) return -1;
    unsigned char b = 0;
    ssize_t n = ::read(STDIN_FILENO, &b, 1);
    if (n != 1) return -1;
    return static_cast<int>(b);
}

// 根据 UTF-8 leading byte 返回剩余字节数(0 表示单字节 / 不是合法 leading)。
int utf8_trailing_bytes(unsigned char lead) {
    if ((lead & 0x80) == 0)        return 0;
    if ((lead & 0xE0) == 0xC0)     return 1;
    if ((lead & 0xF0) == 0xE0)     return 2;
    if ((lead & 0xF8) == 0xF0)     return 3;
    return 0;  // 非法 leading;phase 1 当单字节处理
}

// 把 UTF-8 序列解码成 codepoint。lead 是已读到的首字节;后续字节通过
// continuation_reader 拉取。失败返 0。
char32_t decode_utf8(unsigned char lead,
                     int trailing,
                     int (*continuation_reader)(int)) {
    char32_t cp;
    switch (trailing) {
        case 0: return static_cast<char32_t>(lead);
        case 1: cp = static_cast<char32_t>(lead & 0x1F); break;
        case 2: cp = static_cast<char32_t>(lead & 0x0F); break;
        case 3: cp = static_cast<char32_t>(lead & 0x07); break;
        default: return 0;
    }
    for (int i = 0; i < trailing; ++i) {
        int b = continuation_reader(50);
        if (b < 0) return 0;
        cp = (cp << 6) | static_cast<char32_t>(b & 0x3F);
    }
    return cp;
}

}  // namespace

std::optional<Event> Terminal::read_event(
    std::optional<std::chrono::milliseconds> timeout) {
    while (true) {
        if (g_winch_flag) {
            g_winch_flag = 0;
            Size sz = Terminal::size();
            return Event{ResizeEvent{sz.width, sz.height}};
        }

        int wait_ms = timeout.has_value()
                          ? static_cast<int>(timeout->count())
                          : -1;
        int first = read_byte_with_timeout(wait_ms);
        if (first < 0) {
            // 超时或被 SIGWINCH 打断;loop 顶上再检查一次 winch flag。
            if (g_winch_flag) continue;
            return std::nullopt;
        }

        unsigned char b = static_cast<unsigned char>(first);

        // 0x7F (DEL) 或 0x08 (BS) → Backspace
        if (b == 0x7F || b == 0x08) {
            return Event{KeyEvent{key::kBackspace, Modifier::None}};
        }
        // \r 或 \n → Enter(终端 raw mode 一般送 \r,但兼容 \n)
        if (b == 0x0D || b == 0x0A) {
            return Event{KeyEvent{key::kEnter, Modifier::None}};
        }
        // \t → Tab
        if (b == 0x09) {
            return Event{KeyEvent{0x09, Modifier::None}};
        }
        // \x03 → Ctrl+C
        if (b == 0x03) {
            return Event{KeyEvent{U'C', Modifier::Ctrl}};
        }
        // 0x1B → 单 Esc 或 escape sequence。50ms 内无后续字节 → Esc。
        if (b == 0x1B) {
            int next = read_byte_with_timeout(50);
            if (next < 0) {
                return Event{KeyEvent{key::kEsc, Modifier::None}};
            }
            // CSI(`\033[...`)与 SS3(`\033O...`)是方向键 / Home / End /
            // Delete / function key 的常见两种 escape 形式。
            if (next == '[' || next == 'O') {
                // 拉到一个"final byte"(0x40-0x7E)或"~"为止,中间可能有
                // 数字参数。简单收 8 字节内,够覆盖常见 key。
                int  params[4] = {0, 0, 0, 0};
                int  pidx      = 0;
                int  cur_param = 0;
                bool have_num  = false;
                char final_b   = 0;
                for (int i = 0; i < 8; ++i) {
                    int c = read_byte_with_timeout(10);
                    if (c < 0) break;
                    if (c >= '0' && c <= '9') {
                        cur_param = cur_param * 10 + (c - '0');
                        have_num  = true;
                    } else if (c == ';') {
                        if (pidx < 4) params[pidx++] = cur_param;
                        cur_param = 0;
                        have_num  = false;
                    } else {
                        if (have_num && pidx < 4) params[pidx++] = cur_param;
                        final_b = static_cast<char>(c);
                        break;
                    }
                }

                char32_t cp = 0;
                switch (final_b) {
                    case 'A': cp = key::kUp;    break;
                    case 'B': cp = key::kDown;  break;
                    case 'C': cp = key::kRight; break;
                    case 'D': cp = key::kLeft;  break;
                    case 'H': cp = key::kHome;  break;
                    case 'F': cp = key::kEnd;   break;
                    case '~':
                        switch (params[0]) {
                            case 1: case 7: cp = key::kHome;   break;
                            case 4: case 8: cp = key::kEnd;    break;
                            case 3:         cp = key::kDelete; break;
                            default: break;
                        }
                        break;
                    default: break;
                }
                if (cp != 0) {
                    return Event{KeyEvent{cp, Modifier::None}};
                }
                // 未识别 sequence,直接丢弃,继续 loop。
                continue;
            }
            // 其它 escape prefix(Alt+key 等)— phase 1 不处理,丢弃。
            continue;
        }
        // 其它 control code (0x01-0x1A) → Ctrl+letter
        if (b >= 0x01 && b <= 0x1A) {
            char32_t letter = static_cast<char32_t>(b - 1 + 'A');
            return Event{KeyEvent{letter, Modifier::Ctrl}};
        }

        // >= 0x20:UTF-8 起始字节
        int trailing = utf8_trailing_bytes(b);
        char32_t cp  = decode_utf8(b, trailing, &read_byte_with_timeout);
        if (cp == 0) {
            continue;
        }
        return Event{KeyEvent{cp, Modifier::None}};
    }
}

}  // namespace acetui
