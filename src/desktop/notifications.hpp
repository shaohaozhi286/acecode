#pragma once

// Portable native-notification boundary. Windows uses WinToast, macOS Desktop
// uses UserNotifications, and unsupported platforms retain a safe no-op
// backend. The standalone macOS TUI intentionally does not initialize it.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace acecode::desktop {

struct NotifyPayload {
    std::string id;
    std::string workspace_hash;
    std::string session_id;
    std::string title;
    std::string body;
};

enum class NotificationAuthorizationStatus {
    Unknown,
    NotDetermined,
    Requesting,
    Denied,
    Authorized,
    Provisional,
    Unavailable,
};

struct NotificationAuthorizationState {
    NotificationAuthorizationStatus status =
        NotificationAuthorizationStatus::Unavailable;
    bool can_request = false;
    bool can_open_settings = false;
};

struct NotificationInitOptions {
    std::string app_name = "ACECode";
    // Windows AppUserModelID. Other platforms use their bundle/application
    // identity and ignore this field.
    std::string application_id;
    // Windows: HWND. macOS Desktop: NSWindow*. Unsupported platforms: ignored.
    void* activation_window = nullptr;
};

using ClickHandler = std::function<void(const NotifyPayload& payload)>;
using NotificationAuthorizationHandler =
    std::function<void(const NotificationAuthorizationState& state)>;

// Initializes the current platform backend. Failure is non-fatal and is
// reported as false; later operations remain safe no-ops.
bool init_notifications(const NotificationInitOptions& options);

// The handler is invoked after the native window has been restored and
// foregrounded. Passing nullptr disables activation routing.
void set_click_handler(ClickHandler handler);

// Receives asynchronous OS authorization changes. Passing nullptr disables the
// callback. Callers can query the latest snapshot at any time.
void set_notification_authorization_handler(
    NotificationAuthorizationHandler handler);
NotificationAuthorizationState notification_authorization_state();
const char* notification_authorization_status_name(
    NotificationAuthorizationStatus status);
// Requests a fresh asynchronous OS snapshot where supported. The current
// snapshot remains queryable immediately and changes arrive through the
// authorization handler.
bool refresh_notification_authorization();
bool request_notification_authorization();
bool open_notification_settings();

// Shows one notification. Every platform request owns independent routing
// data, so activating an older notification cannot target a newer session.
bool show_notification(const NotifyPayload& payload);

// Clears outstanding notifications and callbacks. Call before destroying any
// state captured by handlers.
void shutdown_notifications();

// Decode WebView bind arguments. Supports a direct object and the legacy
// JSON-string argument currently sent by desktopNotify.js.
std::optional<NotifyPayload> parse_notification_bridge_args(
    const std::string& args_json,
    std::string* error = nullptr);

// Unicode-codepoint-aware truncation used by native TUI notifications.
std::string truncate_notification_text(
    const std::string& text,
    std::size_t max_codepoints = 80);

NotifyPayload build_completion_notification(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& session_title,
    const std::string& final_assistant_text,
    const std::string& locale = "zh-CN");

// Called by platform event handlers. Public to keep payload fidelity and
// lifecycle behavior directly testable without displaying an OS notification.
void dispatch_notification_activation(const NotifyPayload& payload);

// Windows TUI helpers. Other platform backends return safe defaults.
void* capture_tui_notification_window();
bool notification_window_is_foreground(void* native_window);
bool activate_notification_window(void* native_window);

} // namespace acecode::desktop
