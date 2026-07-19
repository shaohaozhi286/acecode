#include "notifications.hpp"

#include "notifications_backend.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <utility>

namespace acecode::desktop {
namespace {

std::mutex g_notification_mu;
ClickHandler g_click_handler;
NotificationAuthorizationHandler g_authorization_handler;
NotificationAuthorizationState g_authorization_state;
void* g_activation_window = nullptr;
bool g_initialized = false;
bool g_accept_authorization_updates = false;
std::atomic<std::uint64_t> g_notification_sequence{0};

void set_parse_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::string trim_ascii(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](char ch) {
                                                return !is_space(
                                                    static_cast<unsigned char>(ch));
                                            }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char ch) {
                                 return !is_space(static_cast<unsigned char>(ch));
                             }).base(),
                value.end());
    return value;
}

std::size_t utf8_codepoint_bytes(const std::string& text, std::size_t offset) {
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1;
    if ((lead & 0xE0u) == 0xC0u) length = 2;
    else if ((lead & 0xF0u) == 0xE0u) length = 3;
    else if ((lead & 0xF8u) == 0xF0u) length = 4;
    if (offset + length > text.size()) return 1;
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0u) != 0x80u) return 1;
    }
    return length;
}

} // namespace

namespace notification_detail {

void publish_authorization_state(NotificationAuthorizationState state) {
    NotificationAuthorizationHandler handler;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        if (!g_accept_authorization_updates) return;
        changed = state.status != g_authorization_state.status ||
                  state.can_request != g_authorization_state.can_request ||
                  state.can_open_settings !=
                      g_authorization_state.can_open_settings;
        g_authorization_state = state;
        if (changed) handler = g_authorization_handler;
    }
    if (handler) handler(state);
}

} // namespace notification_detail

bool init_notifications(const NotificationInitOptions& options) {
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        if (g_initialized) return true;
        g_activation_window = options.activation_window;
        // Backends can publish an initial state synchronously from initialize()
        // or asynchronously immediately afterwards.
        g_accept_authorization_updates = true;
    }

    const bool initialized = notification_backend::initialize(options);
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        g_initialized = initialized;
        if (!initialized) {
            g_accept_authorization_updates = false;
            g_activation_window = nullptr;
            g_authorization_state = {
                NotificationAuthorizationStatus::Unavailable, false, false};
        }
    }
    return initialized;
}

void set_click_handler(ClickHandler handler) {
    std::lock_guard<std::mutex> lock(g_notification_mu);
    g_click_handler = std::move(handler);
}

void set_notification_authorization_handler(
    NotificationAuthorizationHandler handler) {
    std::lock_guard<std::mutex> lock(g_notification_mu);
    g_authorization_handler = std::move(handler);
}

NotificationAuthorizationState notification_authorization_state() {
    std::lock_guard<std::mutex> lock(g_notification_mu);
    return g_authorization_state;
}

const char* notification_authorization_status_name(
    NotificationAuthorizationStatus status) {
    switch (status) {
        case NotificationAuthorizationStatus::Unknown:
            return "unknown";
        case NotificationAuthorizationStatus::NotDetermined:
            return "not_determined";
        case NotificationAuthorizationStatus::Requesting:
            return "requesting";
        case NotificationAuthorizationStatus::Denied:
            return "denied";
        case NotificationAuthorizationStatus::Authorized:
            return "authorized";
        case NotificationAuthorizationStatus::Provisional:
            return "provisional";
        case NotificationAuthorizationStatus::Unavailable:
            return "unavailable";
    }
    return "unavailable";
}

bool refresh_notification_authorization() {
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        if (!g_initialized) return false;
    }
    return notification_backend::refresh_authorization();
}

bool request_notification_authorization() {
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        if (!g_initialized) return false;
    }
    return notification_backend::request_authorization();
}

bool open_notification_settings() {
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        if (!g_initialized) return false;
    }
    return notification_backend::open_settings();
}

bool show_notification(const NotifyPayload& payload) {
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        if (!g_initialized) return false;
    }
    if (payload.title.empty() && payload.body.empty()) return false;
    return notification_backend::show(payload);
}

void shutdown_notifications() {
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        g_initialized = false;
        // Disable publications before backend teardown. Authorization
        // completions already queued by the OS are then harmless even if they
        // arrive while the native delegate is being detached.
        g_accept_authorization_updates = false;
        g_click_handler = nullptr;
        g_authorization_handler = nullptr;
        g_activation_window = nullptr;
        g_authorization_state = {
            NotificationAuthorizationStatus::Unavailable, false, false};
    }
    notification_backend::shutdown();
}

std::string truncate_notification_text(const std::string& text,
                                       std::size_t max_codepoints) {
    if (text.empty() || max_codepoints == 0) return {};
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset < text.size() && count < max_codepoints) {
        offset += utf8_codepoint_bytes(text, offset);
        ++count;
    }
    if (offset >= text.size()) return text;
    return text.substr(0, offset) + u8"…";
}

NotifyPayload build_completion_notification(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& session_title,
    const std::string& final_assistant_text) {
    NotifyPayload payload;
    payload.id = "completion-" +
        (session_id.empty() ? std::string("unknown") : session_id) + "-" +
        std::to_string(g_notification_sequence.fetch_add(1) + 1);
    payload.workspace_hash = workspace_hash;
    payload.session_id = session_id;
    const std::string title = trim_ascii(session_title);
    payload.title = u8"已完成 · " +
        (title.empty() ? std::string(u8"会话") : title);
    const std::string body = trim_ascii(final_assistant_text);
    payload.body = body.empty()
        ? std::string(u8"(空白回合)")
        : truncate_notification_text(body);
    return payload;
}

std::optional<NotifyPayload> parse_notification_bridge_args(
    const std::string& args_json,
    std::string* error) {
    if (error) error->clear();
    try {
        nlohmann::json value = nlohmann::json::parse(args_json);
        if (value.is_array()) {
            if (value.empty()) {
                set_parse_error(error, "notification arguments are empty");
                return std::nullopt;
            }
            value = value.front();
        }
        if (value.is_string()) {
            value = nlohmann::json::parse(value.get<std::string>());
        }
        if (!value.is_object()) {
            set_parse_error(error, "notification payload must be an object");
            return std::nullopt;
        }

        NotifyPayload payload;
        auto copy_string = [&](const char* key, std::string& out) {
            if (value.contains(key) && value[key].is_string()) {
                out = value[key].get<std::string>();
            }
        };
        copy_string("id", payload.id);
        copy_string("workspace_hash", payload.workspace_hash);
        copy_string("session_id", payload.session_id);
        copy_string("title", payload.title);
        copy_string("body", payload.body);
        if (payload.title.empty() && payload.body.empty()) {
            set_parse_error(error, "notification title and body are empty");
            return std::nullopt;
        }
        return payload;
    } catch (const std::exception& e) {
        set_parse_error(error, e.what());
        return std::nullopt;
    }
}

void dispatch_notification_activation(const NotifyPayload& payload) {
    ClickHandler handler;
    void* activation_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        handler = g_click_handler;
        activation_window = g_activation_window;
    }
    if (!handler) return;
    notification_backend::activate_window(activation_window);
    handler(payload);
}

void* capture_tui_notification_window() {
    return notification_backend::capture_tui_window();
}

bool notification_window_is_foreground(void* native_window) {
    return notification_backend::window_is_foreground(native_window);
}

bool activate_notification_window(void* native_window) {
    return notification_backend::activate_window(native_window);
}

} // namespace acecode::desktop
