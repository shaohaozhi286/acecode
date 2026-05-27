#include "clipboard.hpp"

#include "base64.hpp"
#include "encoding.hpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
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

FILE* open_write_pipe(const std::string& command) {
#ifdef _WIN32
    return _popen(command.c_str(), "w");
#else
    return popen(command.c_str(), "w");
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

ClipboardTextWriteResult write_result(ClipboardTextWriteResult::Status status,
                                      std::string detail = {}) {
    ClipboardTextWriteResult r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}

ClipboardImageReadResult image_result(ClipboardImageReadResult::Status status,
                                      std::string bytes = {},
                                      std::string mime_type = {},
                                      std::string detail = {}) {
    ClipboardImageReadResult r;
    r.status = status;
    r.bytes = std::move(bytes);
    r.mime_type = std::move(mime_type);
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

ClipboardTextWriteResult write_windows_clipboard_text(std::string_view text,
                                                      std::size_t max_bytes) {
    if (text.size() > max_bytes) {
        return write_result(ClipboardTextWriteResult::Status::TooLarge);
    }
    std::wstring wide = utf8_to_wide(std::string(text));
    if (!text.empty() && wide.empty()) {
        return write_result(ClipboardTextWriteResult::Status::Unavailable,
                            "UTF-8 conversion failed");
    }

    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) {
        return write_result(ClipboardTextWriteResult::Status::Unavailable,
                            "GlobalAlloc failed");
    }

    void* locked = GlobalLock(handle);
    if (!locked) {
        GlobalFree(handle);
        return write_result(ClipboardTextWriteResult::Status::Unavailable,
                            "GlobalLock failed");
    }
    std::memcpy(locked, wide.c_str(), bytes);
    GlobalUnlock(handle);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(handle);
        return write_result(ClipboardTextWriteResult::Status::Unavailable,
                            "OpenClipboard failed");
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, handle)) {
        CloseClipboard();
        GlobalFree(handle);
        return write_result(ClipboardTextWriteResult::Status::Unavailable,
                            "SetClipboardData failed");
    }
    CloseClipboard();
    return write_result(ClipboardTextWriteResult::Status::Success);
}

ClipboardImageReadResult read_windows_clipboard_image(std::size_t max_bytes) {
    const std::string command =
        "powershell -NoProfile -ExecutionPolicy Bypass -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms;"
        "Add-Type -AssemblyName System.Drawing;"
        "$img=[System.Windows.Forms.Clipboard]::GetImage();"
        "if($null -eq $img){exit 0};"
        "$ms=New-Object System.IO.MemoryStream;"
        "$img.Save($ms,[System.Drawing.Imaging.ImageFormat]::Png);"
        "$bytes=$ms.ToArray();"
        "if($bytes.Length -gt " + std::to_string(max_bytes) + "){exit 3};"
        "[Convert]::ToBase64String($bytes)\"";

    auto read = read_system_clipboard_text_from_commands({command}, max_bytes * 2);
    if (read.status == ClipboardTextReadResult::Status::Empty) {
        return image_result(ClipboardImageReadResult::Status::Empty);
    }
    if (read.status == ClipboardTextReadResult::Status::TooLarge) {
        return image_result(ClipboardImageReadResult::Status::TooLarge);
    }
    if (!read) {
        return image_result(ClipboardImageReadResult::Status::Unavailable,
                            {},
                            {},
                            read.detail);
    }

    std::string b64 = read.text;
    b64.erase(std::remove_if(b64.begin(), b64.end(), [](unsigned char c) {
        return c == '\r' || c == '\n' || c == ' ' || c == '\t';
    }), b64.end());
    auto decoded = base64_decode(b64);
    if (!decoded.has_value()) {
        return image_result(ClipboardImageReadResult::Status::Unavailable,
                            {},
                            {},
                            "clipboard image base64 decode failed");
    }
    if (decoded->size() > max_bytes) {
        return image_result(ClipboardImageReadResult::Status::TooLarge);
    }
    return image_result(ClipboardImageReadResult::Status::Success,
                        std::move(*decoded),
                        "image/png");
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

std::vector<std::string> linux_clipboard_write_commands(bool has_wayland_display,
                                                        bool has_x11_display) {
    std::vector<std::string> commands;
    if (has_wayland_display) {
        commands.push_back("wl-copy --type text/plain 2>/dev/null");
    }
    if (has_x11_display) {
        commands.push_back("xclip -selection clipboard 2>/dev/null");
        commands.push_back("xsel --clipboard --input 2>/dev/null");
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

ClipboardTextWriteResult write_system_clipboard_text_from_commands(
    const std::vector<std::string>& commands,
    std::string_view text,
    std::size_t max_bytes) {
    if (text.size() > max_bytes) {
        return write_result(ClipboardTextWriteResult::Status::TooLarge);
    }
    if (commands.empty()) {
        return write_result(ClipboardTextWriteResult::Status::Unavailable,
                            "No clipboard command candidates");
    }

    for (const auto& command : commands) {
        FILE* pipe = open_write_pipe(command);
        if (!pipe) {
            continue;
        }

        const std::size_t written =
            text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), pipe);
        const bool write_ok = written == text.size();
        const int status = close_pipe(pipe);
        if (write_ok && command_succeeded(status)) {
            return write_result(ClipboardTextWriteResult::Status::Success);
        }
    }

    return write_result(ClipboardTextWriteResult::Status::Unavailable,
                        "No clipboard command succeeded");
}

ClipboardImageReadResult read_system_clipboard_image_from_commands(
    const std::vector<std::string>& commands,
    std::string mime_type,
    std::size_t max_bytes) {
    if (commands.empty()) {
        return image_result(ClipboardImageReadResult::Status::Unavailable,
                            {},
                            {},
                            "No clipboard command candidates");
    }

    bool saw_empty_clipboard = false;
    for (const auto& command : commands) {
        FILE* pipe = open_pipe(command);
        if (!pipe) continue;

        std::string output;
        output.reserve(8192);
        bool too_large = false;
        std::array<char, 8192> buffer{};
        while (true) {
            const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe);
            if (n > 0) {
                const std::size_t remaining =
                    output.size() < max_bytes ? max_bytes - output.size() : 0;
                if (remaining > 0) {
                    output.append(buffer.data(), std::min(n, remaining));
                }
                if (n > remaining) too_large = true;
            }
            if (n < buffer.size()) {
                if (std::feof(pipe) || std::ferror(pipe)) break;
            }
        }

        const int status = close_pipe(pipe);
        if (!command_succeeded(status)) continue;
        if (too_large) return image_result(ClipboardImageReadResult::Status::TooLarge);
        if (!output.empty()) {
            return image_result(ClipboardImageReadResult::Status::Success,
                                std::move(output),
                                std::move(mime_type));
        }
        saw_empty_clipboard = true;
    }

    if (saw_empty_clipboard) return image_result(ClipboardImageReadResult::Status::Empty);
    return image_result(ClipboardImageReadResult::Status::Unavailable,
                        {},
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

ClipboardImageReadResult read_system_clipboard_image(std::size_t max_bytes) {
#ifdef _WIN32
    return read_windows_clipboard_image(max_bytes);
#elif defined(__APPLE__)
    return read_system_clipboard_image_from_commands({"pngpaste - 2>/dev/null"},
                                                     "image/png",
                                                     max_bytes);
#else
    std::vector<std::string> commands;
    if (has_env_value("WAYLAND_DISPLAY")) {
        commands.push_back("wl-paste --no-newline --type image/png 2>/dev/null");
    }
    if (has_env_value("DISPLAY")) {
        commands.push_back("xclip -selection clipboard -t image/png -o 2>/dev/null");
    }
    return read_system_clipboard_image_from_commands(commands, "image/png", max_bytes);
#endif
}

ClipboardTextWriteResult write_system_clipboard_text(std::string_view text,
                                                     std::size_t max_bytes) {
#ifdef _WIN32
    return write_windows_clipboard_text(text, max_bytes);
#elif defined(__APPLE__)
    return write_system_clipboard_text_from_commands({"pbcopy 2>/dev/null"},
                                                     text,
                                                     max_bytes);
#else
    return write_system_clipboard_text_from_commands(
        linux_clipboard_write_commands(has_env_value("WAYLAND_DISPLAY"),
                                       has_env_value("DISPLAY")),
        text,
        max_bytes);
#endif
}

} // namespace acecode
