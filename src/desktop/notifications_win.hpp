#pragma once

// Shared Windows notification boundary for acecode.exe (TUI) and
// acecode-desktop.exe. The Windows implementation uses WinToast; other
// platforms keep the same API as a no-op so callers do not need platform
// conditionals around notification lifecycle calls.

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

struct NotificationInitOptions {
    std::wstring app_name = L"ACECode";
    std::wstring app_user_model_id;
    void* activation_window = nullptr; // Windows: HWND
};

using ClickHandler = std::function<void(const NotifyPayload& payload)>;

// Initializes WinToast for the current process. Failure is non-fatal and is
// reported as false; show_notification then remains a safe no-op.
bool init_notifications(const NotificationInitOptions& options);

// The handler is invoked after the native window has been restored and
// foregrounded. Passing nullptr disables activation routing.
void set_click_handler(ClickHandler handler);

// Shows one notification. Every toast owns an independent payload handler, so
// activation of an older toast cannot be redirected to the newest session.
bool show_notification(const NotifyPayload& payload);

// Clears outstanding toasts and activation callbacks. Call before destroying
// any state captured by the click handler.
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
    const std::string& final_assistant_text);

// Called by each WinToast event handler. Public to make payload fidelity and
// lifecycle behavior directly testable without displaying an OS toast.
void dispatch_notification_activation(const NotifyPayload& payload);

// Windows Terminal can expose an invisible pseudo console HWND. Prefer the
// visible console window, then fall back to the foreground terminal captured
// during TUI startup.
void* capture_tui_notification_window();
bool notification_window_is_foreground(void* native_window);
bool activate_notification_window(void* native_window);

} // namespace acecode::desktop
