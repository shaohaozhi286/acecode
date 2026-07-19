#include "notifications_backend.hpp"

#if !defined(_WIN32) && !defined(__APPLE__)

namespace acecode::desktop::notification_backend {

bool initialize(const NotificationInitOptions& /*options*/) {
    notification_detail::publish_authorization_state({
        NotificationAuthorizationStatus::Unavailable, false, false});
    return false;
}

bool show(const NotifyPayload& /*payload*/) {
    return false;
}

void shutdown() {}

bool refresh_authorization() {
    return false;
}

bool request_authorization() {
    return false;
}

bool open_settings() {
    return false;
}

void* capture_tui_window() {
    return nullptr;
}

bool window_is_foreground(void* /*native_window*/) {
    return false;
}

bool activate_window(void* /*native_window*/) {
    return false;
}

} // namespace acecode::desktop::notification_backend

#endif
