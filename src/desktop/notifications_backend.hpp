#pragma once

#include "notifications.hpp"

namespace acecode::desktop::notification_backend {

bool initialize(const NotificationInitOptions& options);
bool show(const NotifyPayload& payload);
void shutdown();

bool refresh_authorization();
bool request_authorization();
bool open_settings();

void* capture_tui_window();
bool window_is_foreground(void* native_window);
bool activate_window(void* native_window);

} // namespace acecode::desktop::notification_backend

namespace acecode::desktop::notification_detail {

// Platform backends publish authoritative OS state through this hook. It
// updates the portable snapshot and invokes the registered application handler
// after releasing the facade mutex.
void publish_authorization_state(NotificationAuthorizationState state);

} // namespace acecode::desktop::notification_detail
