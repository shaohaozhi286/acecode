#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace acecode {

inline constexpr std::size_t kMaxClipboardTextBytes = 5 * 1024 * 1024;
inline constexpr std::size_t kMaxClipboardImageBytes = 25 * 1024 * 1024;

struct ClipboardTextReadResult {
    enum class Status {
        Success,
        Empty,
        Unavailable,
        TooLarge,
    };

    Status status = Status::Unavailable;
    std::string text;
    std::string detail;

    explicit operator bool() const noexcept {
        return status == Status::Success;
    }
};

struct ClipboardTextWriteResult {
    enum class Status {
        Success,
        Unavailable,
        TooLarge,
    };

    Status status = Status::Unavailable;
    std::string detail;

    explicit operator bool() const noexcept {
        return status == Status::Success;
    }
};

struct ClipboardImageReadResult {
    enum class Status {
        Success,
        Empty,
        Unavailable,
        TooLarge,
    };

    Status status = Status::Unavailable;
    std::string bytes;
    std::string mime_type;
    std::string detail;

    explicit operator bool() const noexcept {
        return status == Status::Success;
    }
};

std::vector<std::string> linux_clipboard_text_commands(bool has_wayland_display,
                                                       bool has_x11_display);

std::vector<std::string> linux_clipboard_write_commands(bool has_wayland_display,
                                                        bool has_x11_display);

ClipboardTextReadResult read_system_clipboard_text(
    std::size_t max_bytes = kMaxClipboardTextBytes);

ClipboardTextWriteResult write_system_clipboard_text(
    std::string_view text,
    std::size_t max_bytes = kMaxClipboardTextBytes);

ClipboardImageReadResult read_system_clipboard_image(
    std::size_t max_bytes = kMaxClipboardImageBytes);

ClipboardTextReadResult read_system_clipboard_text_from_commands(
    const std::vector<std::string>& commands,
    std::size_t max_bytes = kMaxClipboardTextBytes);

ClipboardTextWriteResult write_system_clipboard_text_from_commands(
    const std::vector<std::string>& commands,
    std::string_view text,
    std::size_t max_bytes = kMaxClipboardTextBytes);

ClipboardImageReadResult read_system_clipboard_image_from_commands(
    const std::vector<std::string>& commands,
    std::string mime_type,
    std::size_t max_bytes = kMaxClipboardImageBytes);

} // namespace acecode
