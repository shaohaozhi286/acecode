#pragma once

#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace acecode::upgrade {

enum class ConsoleStyle {
    Bold,
    Dim,
    Cyan,
    Green,
    Yellow,
    Red,
};

inline int console_fd_for_stream(std::ostream& out) {
    if (out.rdbuf() == std::cout.rdbuf()) {
#ifdef _WIN32
        return _fileno(stdout);
#else
        return fileno(stdout);
#endif
    }
    if (out.rdbuf() == std::cerr.rdbuf() || out.rdbuf() == std::clog.rdbuf()) {
#ifdef _WIN32
        return _fileno(stderr);
#else
        return fileno(stderr);
#endif
    }
    return -1;
}

inline bool console_fd_is_tty(int fd) {
    if (fd < 0) return false;
#ifdef _WIN32
    return _isatty(fd) != 0;
#else
    return isatty(fd) != 0;
#endif
}

inline bool console_stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

inline bool stream_is_interactive_terminal(std::ostream& out) {
    return console_fd_is_tty(console_fd_for_stream(out));
}

#ifdef _WIN32
inline bool enable_virtual_terminal_for_fd(int fd) {
    if (fd < 0) return false;
    intptr_t os_handle = _get_osfhandle(fd);
    if (os_handle == -1) return false;
    HANDLE handle = reinterpret_cast<HANDLE>(os_handle);
    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) return false;
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) return true;
    return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#endif

inline bool stream_supports_color(std::ostream& out) {
    if (std::getenv("NO_COLOR") != nullptr) return false;
    int fd = console_fd_for_stream(out);
#ifdef _WIN32
    return console_fd_is_tty(fd) && enable_virtual_terminal_for_fd(fd);
#else
    return console_fd_is_tty(fd);
#endif
}

inline const char* ansi_code(ConsoleStyle style) {
    switch (style) {
    case ConsoleStyle::Bold: return "\x1b[1m";
    case ConsoleStyle::Dim: return "\x1b[2m";
    case ConsoleStyle::Cyan: return "\x1b[36m";
    case ConsoleStyle::Green: return "\x1b[32m";
    case ConsoleStyle::Yellow: return "\x1b[33m";
    case ConsoleStyle::Red: return "\x1b[31m";
    }
    return "";
}

inline std::string styled(std::ostream& out, ConsoleStyle style, const std::string& text) {
    if (!stream_supports_color(out)) return text;
    return std::string(ansi_code(style)) + text + "\x1b[0m";
}

inline void prompt_press_any_key_if_interactive(std::ostream& out) {
    if (out.rdbuf() != std::cout.rdbuf() || !stream_is_interactive_terminal(out) ||
        !console_stdin_is_tty()) {
        return;
    }

    out << "\nPress any key to continue..." << std::flush;
#ifdef _WIN32
    (void)_getch();
#else
    termios old_term{};
    if (tcgetattr(STDIN_FILENO, &old_term) == 0) {
        termios new_term = old_term;
        new_term.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
        char ch = 0;
        (void)read(STDIN_FILENO, &ch, 1);
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    } else {
        char ch = 0;
        (void)read(STDIN_FILENO, &ch, 1);
    }
#endif
    out << "\n" << std::flush;
}

} // namespace acecode::upgrade
