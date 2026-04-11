#pragma once

#include <string>
#include <vector>
#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <termios.h>
#endif

namespace acecode {

// Read a line from stdin with prompt and default value.
// Shows "[default]" in prompt. Returns default on empty input.
inline std::string read_line(const std::string& prompt, const std::string& default_value = "") {
    if (default_value.empty()) {
        std::cout << prompt << ": ";
    } else {
        std::cout << prompt << " [" << default_value << "]: ";
    }
    std::cout << std::flush;

    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) {
        return default_value;
    }
    return input;
}

// Read a password (no echo). Shows masked hint of existing value.
// On non-TTY, falls back to normal input with warning.
inline std::string read_password(const std::string& prompt, const std::string& existing = "") {
    // Build display hint for existing value
    std::string hint;
    if (!existing.empty()) {
        if (existing.size() > 4) {
            hint = "****" + existing.substr(existing.size() - 4);
        } else {
            hint = "****";
        }
    }

    if (hint.empty()) {
        std::cout << prompt << ": ";
    } else {
        std::cout << prompt << " [" << hint << "]: ";
    }
    std::cout << std::flush;

#ifdef _WIN32
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    bool is_tty = (_isatty(_fileno(stdin)) != 0);
    DWORD old_mode = 0;

    if (is_tty) {
        GetConsoleMode(hStdin, &old_mode);
        SetConsoleMode(hStdin, old_mode & ~ENABLE_ECHO_INPUT);
    } else {
        std::cerr << "Warning: input may be visible" << std::endl;
    }

    std::string input;
    std::getline(std::cin, input);

    if (is_tty) {
        SetConsoleMode(hStdin, old_mode);
        std::cout << std::endl; // newline after hidden input
    }
#else
    bool is_tty = (isatty(fileno(stdin)) != 0);
    struct termios old_term, new_term;

    if (is_tty) {
        tcgetattr(fileno(stdin), &old_term);
        new_term = old_term;
        new_term.c_lflag &= ~ECHO;
        tcsetattr(fileno(stdin), TCSANOW, &new_term);
    } else {
        std::cerr << "Warning: input may be visible" << std::endl;
    }

    std::string input;
    std::getline(std::cin, input);

    if (is_tty) {
        tcsetattr(fileno(stdin), TCSANOW, &old_term);
        std::cout << std::endl;
    }
#endif

    if (input.empty()) {
        return existing;
    }
    return input;
}

// Display numbered menu and read user choice.
// Returns 0-based index. default_index is 0-based (-1 = no default).
inline int read_choice(const std::string& prompt,
                       const std::vector<std::string>& options,
                       int default_index = -1) {
    std::cout << prompt << std::endl;
    for (size_t i = 0; i < options.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << options[i];
        if (static_cast<int>(i) == default_index) {
            std::cout << " (default)";
        }
        std::cout << std::endl;
    }

    while (true) {
        if (default_index >= 0) {
            std::cout << "Choose [" << (default_index + 1) << "]: ";
        } else {
            std::cout << "Choose: ";
        }
        std::cout << std::flush;

        std::string input;
        std::getline(std::cin, input);

        if (input.empty() && default_index >= 0) {
            return default_index;
        }

        try {
            int choice = std::stoi(input);
            if (choice >= 1 && choice <= static_cast<int>(options.size())) {
                return choice - 1;
            }
        } catch (...) {}

        std::cout << "Invalid choice. Please enter a number between 1 and "
                  << options.size() << "." << std::endl;
    }
}

// Simple yes/no prompt. Returns true for yes.
inline bool read_confirm(const std::string& prompt, bool default_yes = true) {
    std::string suffix = default_yes ? " [Y/n]: " : " [y/N]: ";
    std::cout << prompt << suffix << std::flush;

    std::string input;
    std::getline(std::cin, input);

    if (input.empty()) {
        return default_yes;
    }
    return (input[0] == 'y' || input[0] == 'Y');
}

} // namespace acecode
