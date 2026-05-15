#include "clipboard.hpp"

#include "encoding.hpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace acecode {

namespace {

bool has_env_value(const char* name) {
    const char* value = std::getenv(name);
    return value && *value;
}

FILE* open_pipe(const std::string& command) {
#ifdef _WIN32
    return _popen(command.c_str(), "r");
#else
    return popen(command.c_str(), "r");
#endif
}

int close_pipe(FILE* pipe) {
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

bool command_succeeded(int status) {
#ifdef _WIN32
    return status == 0;
#else
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

ClipboardTextReadResult result(ClipboardTextReadResult::Status status,
                               std::string text = {},
                               std::string detail = {}) {
    ClipboardTextReadResult r;
    r.status = status;
    r.text = std::move(text);
    r.detail = std::move(detail);
    return r;
}

#ifdef _WIN32
ClipboardTextReadResult read_windows_clipboard_text(std::size_t max_bytes) {
    if (!OpenClipboard(nullptr)) {
        return result(ClipboardTextReadResult::Status::Unavailable,
                      {},
                      "OpenClipboard failed");
    }

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        CloseClipboard();
        return result(ClipboardTextReadResult::Status::Empty);
    }

    const wchar_t* locked = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!locked) {
        CloseClipboard();
        return result(ClipboardTextReadResult::Status::Unavailable,
                      {},
                      "GlobalLock failed");
    }

    std::wstring wide(locked);
    GlobalUnlock(handle);
    CloseClipboard();

    std::string text = wide_to_utf8(wide);
    if (text.empty()) {
        return result(ClipboardTextReadResult::Status::Empty);
    }
    if (text.size() > max_bytes) {
        return result(ClipboardTextReadResult::Status::TooLarge);
    }
    return result(ClipboardTextReadResult::Status::Success, std::move(text));
}
#endif

} // namespace

std::vector<std::string> linux_clipboard_text_commands(bool has_wayland_display,
                                                       bool has_x11_display) {
    std::vector<std::string> commands;
    if (has_wayland_display) {
        commands.push_back("wl-paste --no-newline --type text 2>/dev/null");
    }
    if (has_x11_display) {
        commands.push_back("xclip -selection clipboard -o 2>/dev/null");
        commands.push_back("xsel --clipboard --output 2>/dev/null");
        commands.push_back("xclip -selection primary -o 2>/dev/null");
        commands.push_back("xsel --primary --output 2>/dev/null");
    }
    return commands;
}

ClipboardTextReadResult read_system_clipboard_text_from_commands(
    const std::vector<std::string>& commands,
    std::size_t max_bytes) {
    if (commands.empty()) {
        return result(ClipboardTextReadResult::Status::Unavailable,
                      {},
                      "No clipboard command candidates");
    }

    bool saw_empty_clipboard = false;
    for (const auto& command : commands) {
        FILE* pipe = open_pipe(command);
        if (!pipe) {
            continue;
        }

        std::string output;
        output.reserve(4096);
        bool too_large = false;
        std::array<char, 4096> buffer{};
        while (true) {
            const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe);
            if (n > 0) {
                const std::size_t remaining =
                    output.size() < max_bytes ? max_bytes - output.size() : 0;
                if (remaining > 0) {
                    output.append(buffer.data(), std::min(n, remaining));
                }
                if (n > remaining) {
                    too_large = true;
                }
            }
            if (n < buffer.size()) {
                if (std::feof(pipe) || std::ferror(pipe)) {
                    break;
                }
            }
        }

        const int status = close_pipe(pipe);
        if (!command_succeeded(status)) {
            continue;
        }
        if (too_large) {
            return result(ClipboardTextReadResult::Status::TooLarge);
        }

        output = ensure_utf8(output);
        if (!output.empty()) {
            return result(ClipboardTextReadResult::Status::Success,
                          std::move(output));
        }
        saw_empty_clipboard = true;
    }

    if (saw_empty_clipboard) {
        return result(ClipboardTextReadResult::Status::Empty);
    }
    return result(ClipboardTextReadResult::Status::Unavailable,
                  {},
                  "No clipboard command succeeded");
}

ClipboardTextReadResult read_system_clipboard_text(std::size_t max_bytes) {
#ifdef _WIN32
    return read_windows_clipboard_text(max_bytes);
#elif defined(__APPLE__)
    return read_system_clipboard_text_from_commands({"pbpaste 2>/dev/null"},
                                                    max_bytes);
#else
    return read_system_clipboard_text_from_commands(
        linux_clipboard_text_commands(has_env_value("WAYLAND_DISPLAY"),
                                      has_env_value("DISPLAY")),
        max_bytes);
#endif
}

} // namespace acecode
