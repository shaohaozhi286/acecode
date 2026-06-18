#include "tui/clipboard_helpers.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <nlohmann/json.hpp>

#include "tui_state.hpp"
#include "session/attachment_store.hpp"
#include "utils/clipboard.hpp"

namespace acecode { namespace tui {

using namespace ftxui;

void set_transient_status_line_locked(TuiState& state, const std::string& message) {
    if (state.status_line_clear_at.time_since_epoch().count() == 0) {
        state.status_line_saved = state.status_line;
    }
    state.status_line = message;
    state.status_line_clear_at =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
}

std::string clipboard_paste_status_message(ClipboardTextReadResult::Status status) {
    using Status = ClipboardTextReadResult::Status;
    switch (status) {
        case Status::Empty:      return "Clipboard is empty";
        case Status::TooLarge:   return "Clipboard text too large (max " +
                                        std::to_string(kMaxClipboardTextBytes / (1024 * 1024)) + " MB)";
        case Status::Unavailable:
#ifdef _WIN32
            return "Clipboard paste unavailable";
#elif defined(__APPLE__)
            return "Clipboard paste unavailable (pbpaste failed)";
#else
            return "Clipboard paste unavailable (install wl-clipboard, xclip, or xsel)";
#endif
        case Status::Success:
        default:                 return "";
    }
}

std::string clipboard_image_status_message(ClipboardImageReadResult::Status status) {
    using Status = ClipboardImageReadResult::Status;
    switch (status) {
        case Status::Empty:      return "Clipboard has no image";
        case Status::TooLarge:   return "Clipboard image too large (max " +
                                        std::to_string(kMaxClipboardImageBytes / (1024 * 1024)) + " MB)";
        case Status::Unavailable:
#ifdef _WIN32
            return "Clipboard image paste unavailable";
#elif defined(__APPLE__)
            return "Clipboard image paste unavailable (install pngpaste)";
#else
            return "Clipboard image paste unavailable (install wl-clipboard or xclip)";
#endif
        case Status::Success:
        default:                 return "";
    }
}

std::string clipboard_copy_status_message(ClipboardTextWriteResult::Status status) {
    using Status = ClipboardTextWriteResult::Status;
    switch (status) {
        case Status::TooLarge:   return "Clipboard text too large (max " +
                                        std::to_string(kMaxClipboardTextBytes / (1024 * 1024)) + " MB)";
        case Status::Unavailable:
#ifdef _WIN32
            return "Clipboard copy unavailable";
#elif defined(__APPLE__)
            return "Clipboard copy unavailable (pbcopy failed)";
#else
            return "Clipboard copy unavailable (install wl-clipboard, xclip, or xsel)";
#endif
        case Status::Success:
        default:                 return "";
    }
}

bool is_alt_v_event(const Event& event) {
    return event == Event::Special("\x1Bv") || event == Event::Special("\x1BV");
}

bool is_alt_a_event(const Event& event) {
    return event == Event::Special("\x1B" "a") || event == Event::Special("\x1B" "A");
}

std::string attachment_name_from_json(const nlohmann::json& attachment) {
    return attachment.value("name", std::string{"attachment"});
}

std::string display_prompt_with_attachments(const std::string& prompt,
                                            const std::vector<nlohmann::json>& attachments) {
    std::string display = prompt;
    for (const auto& attachment : attachments) {
        if (!display.empty()) display.push_back('\n');
        const std::string kind = attachment.value("kind", std::string{"file"});
        display += "[";
        display += (kind == "image") ? "Image: " : "File: ";
        display += attachment_name_from_json(attachment);
        display += "]";
    }
    return display;
}

UserInput build_user_input_with_attachments(const std::string& prompt,
                                            const std::string& display_text,
                                            const std::vector<nlohmann::json>& attachments) {
    UserInput input;
    input.text = prompt;
    input.display_text = display_text;
    input.content_parts = nlohmann::json::array();
    if (!prompt.empty()) {
        input.content_parts.push_back({{"type", "text"}, {"text", prompt}});
    }
    nlohmann::json attachment_meta = nlohmann::json::array();
    for (const auto& attachment : attachments) {
        const std::string part_kind = attachment_kind_for_mime(
            attachment.value("mime_type", std::string{}),
            attachment.value("name", std::string{}));
        input.content_parts.push_back({
            {"type", part_kind == "image" ? "image" : "file"},
            {"attachment", attachment},
        });
        attachment_meta.push_back(attachment);
    }
    if (attachment_meta.empty()) {
        input.content_parts = nlohmann::json::array();
    } else {
        input.metadata["attachments"] = std::move(attachment_meta);
    }
    return input;
}

}} // namespace acecode::tui
