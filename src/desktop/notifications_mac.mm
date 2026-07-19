#include "notifications_backend.hpp"

#ifdef __APPLE__

#include "../utils/logger.hpp"

#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace acecode::desktop::notification_backend {

void handle_notification_response(UNNotificationResponse* response);
void handle_foreground_notification(
    UNNotification* notification,
    void (^completion_handler)(UNNotificationPresentationOptions options));

} // namespace acecode::desktop::notification_backend

@interface ACECodeNotificationCenterDelegate
    : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation ACECodeNotificationCenterDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:
             (void (^)(UNNotificationPresentationOptions options))completionHandler {
    (void)center;
    acecode::desktop::notification_backend::handle_foreground_notification(
        notification, completionHandler);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
    (void)center;
    acecode::desktop::notification_backend::handle_notification_response(
        response);
    completionHandler();
}

@end

namespace acecode::desktop::notification_backend {
namespace {

std::mutex g_mac_mu;
UNUserNotificationCenter* g_center = nil;
ACECodeNotificationCenterDelegate* g_delegate = nil;
bool g_initialized = false;
bool g_authorization_request_in_flight = false;
std::uint64_t g_generation = 0;
std::optional<NotifyPayload> g_pending_payload;
std::atomic<std::uint64_t> g_fallback_identifier{0};

NSString* utf8_string(const std::string& value, NSString* fallback = @"") {
    if (value.empty()) return fallback;
    NSString* converted = [[[NSString alloc]
        initWithBytes:value.data()
               length:value.size()
             encoding:NSUTF8StringEncoding] autorelease];
    return converted ?: fallback;
}

std::string utf8_value(id value) {
    if (![value isKindOfClass:[NSString class]]) return {};
    const char* text = [(NSString*)value UTF8String];
    return text ? std::string(text) : std::string();
}

NotificationAuthorizationState authorization_state_for_status(
    UNAuthorizationStatus status) {
    switch (status) {
        case UNAuthorizationStatusNotDetermined:
            return {NotificationAuthorizationStatus::NotDetermined, true, true};
        case UNAuthorizationStatusDenied:
            return {NotificationAuthorizationStatus::Denied, false, true};
        case UNAuthorizationStatusAuthorized:
            return {NotificationAuthorizationStatus::Authorized, false, true};
        case UNAuthorizationStatusProvisional:
            return {NotificationAuthorizationStatus::Provisional, false, true};
        default:
            return {NotificationAuthorizationStatus::Unavailable, false, true};
    }
}

NotifyPayload payload_from_notification(UNNotification* notification) {
    NotifyPayload payload;
    NSDictionary* user_info =
        notification.request.content.userInfo;
    if (![user_info isKindOfClass:[NSDictionary class]]) return payload;
    payload.id = utf8_value([user_info objectForKey:@"id"]);
    payload.workspace_hash =
        utf8_value([user_info objectForKey:@"workspace_hash"]);
    payload.session_id =
        utf8_value([user_info objectForKey:@"session_id"]);
    payload.title = utf8_value([user_info objectForKey:@"title"]);
    payload.body = utf8_value([user_info objectForKey:@"body"]);
    return payload;
}

bool generation_is_current(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(g_mac_mu);
    return g_initialized && generation == g_generation;
}

bool submit_authorized_notification(const NotifyPayload& payload) {
    @autoreleasepool {
        // Keep submission serialized with shutdown. Once addNotificationRequest
        // returns, a concurrent shutdown will remove this request; if shutdown
        // wins first, the request is rejected here.
        std::lock_guard<std::mutex> lock(g_mac_mu);
        if (!g_initialized || !g_center) return false;

        UNMutableNotificationContent* content =
            [[[UNMutableNotificationContent alloc] init] autorelease];
        content.title = utf8_string(
            payload.title.empty() ? std::string("ACECode") : payload.title,
            @"ACECode");
        content.body = utf8_string(payload.body);
        content.sound = [UNNotificationSound defaultSound];
        content.userInfo = @{
            @"id": utf8_string(payload.id),
            @"workspace_hash": utf8_string(payload.workspace_hash),
            @"session_id": utf8_string(payload.session_id),
            @"title": utf8_string(payload.title),
            @"body": utf8_string(payload.body),
        };

        std::string identifier = payload.id.empty()
            ? std::string("acecode-notification")
            : payload.id;
        identifier += "-" +
            std::to_string(g_fallback_identifier.fetch_add(1) + 1);
        UNNotificationRequest* request =
            [UNNotificationRequest
                requestWithIdentifier:utf8_string(identifier, @"acecode-notification")
                            content:content
                            trigger:nil];
        [g_center addNotificationRequest:request
                   withCompletionHandler:^(NSError* error) {
                       if (error) {
                           const char* detail =
                               [[error localizedDescription] UTF8String];
                           LOG_WARN(std::string(
                                        "[notifications] macOS delivery failed") +
                                    (detail
                                         ? ": " + std::string(detail)
                                         : ""));
                       }
                   }];
    }
    return true;
}

void apply_authorization_settings(UNNotificationSettings* settings,
                                  std::uint64_t generation) {
    if (!settings) return;

    NotificationAuthorizationState next =
        authorization_state_for_status(settings.authorizationStatus);
    std::optional<NotifyPayload> pending;
    {
        std::lock_guard<std::mutex> lock(g_mac_mu);
        if (!g_initialized || generation != g_generation) return;

        if (g_authorization_request_in_flight &&
            next.status == NotificationAuthorizationStatus::NotDetermined) {
            next = {
                NotificationAuthorizationStatus::Requesting, false, true};
        } else {
            g_authorization_request_in_flight = false;
        }

        if (next.status == NotificationAuthorizationStatus::Authorized ||
            next.status == NotificationAuthorizationStatus::Provisional) {
            pending = std::move(g_pending_payload);
            g_pending_payload.reset();
        } else if (next.status == NotificationAuthorizationStatus::Denied ||
                   next.status ==
                       NotificationAuthorizationStatus::Unavailable) {
            g_pending_payload.reset();
        }
    }

    notification_detail::publish_authorization_state(next);
    if (pending.has_value() && generation_is_current(generation)) {
        submit_authorized_notification(*pending);
    }
}

void refresh_authorization_settings(UNUserNotificationCenter* center,
                                    std::uint64_t generation) {
    [center getNotificationSettingsWithCompletionHandler:
        ^(UNNotificationSettings* settings) {
            apply_authorization_settings(settings, generation);
        }];
}

} // namespace

bool initialize(const NotificationInitOptions& /*options*/) {
    @autoreleasepool {
        NSString* bundle_identifier = [[NSBundle mainBundle] bundleIdentifier];
        if (!bundle_identifier || [bundle_identifier length] == 0) {
            LOG_WARN("[notifications] macOS initialization skipped: "
                     "main bundle has no identifier");
            return false;
        }

        UNUserNotificationCenter* center =
            [UNUserNotificationCenter currentNotificationCenter];
        if (!center) {
            LOG_WARN("[notifications] macOS notification center unavailable");
            return false;
        }

        ACECodeNotificationCenterDelegate* delegate =
            [[ACECodeNotificationCenterDelegate alloc] init];
        if (!delegate) return false;

        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(g_mac_mu);
            if (g_initialized) {
                [delegate release];
                return true;
            }
            g_center = center;
            g_delegate = delegate;
            g_initialized = true;
            g_authorization_request_in_flight = false;
            g_pending_payload.reset();
            generation = ++g_generation;
        }

        center.delegate = delegate;
        notification_detail::publish_authorization_state({
            NotificationAuthorizationStatus::Unknown, true, true});
        refresh_authorization_settings(center, generation);
        LOG_INFO("[notifications] macOS UserNotifications initialized for " +
                 utf8_value(bundle_identifier));
        return true;
    }
}

bool refresh_authorization() {
    UNUserNotificationCenter* center = nil;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mac_mu);
        if (!g_initialized || !g_center) return false;
        center = g_center;
        generation = g_generation;
    }
    refresh_authorization_settings(center, generation);
    return true;
}

bool request_authorization() {
    const auto current = notification_authorization_state();
    if (current.status == NotificationAuthorizationStatus::Authorized ||
        current.status == NotificationAuthorizationStatus::Provisional) {
        return true;
    }
    if (current.status == NotificationAuthorizationStatus::Denied ||
        current.status == NotificationAuthorizationStatus::Unavailable) {
        return false;
    }

    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_mac_mu);
        if (!g_initialized || !g_center) return false;
        if (g_authorization_request_in_flight) return true;
        g_authorization_request_in_flight = true;
        generation = g_generation;
        // Serialize request submission with shutdown. The completion itself is
        // guarded by the lifecycle generation below.
        [g_center
            requestAuthorizationWithOptions:
                (UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                        completionHandler:^(BOOL /*granted*/, NSError* error) {
                            if (!generation_is_current(generation)) return;
                            {
                                std::lock_guard<std::mutex> lock(g_mac_mu);
                                if (!g_initialized ||
                                    generation != g_generation) {
                                    return;
                                }
                                // A completed request must become retryable if
                                // the OS reports an error and leaves the status
                                // undetermined. The settings refresh below
                                // publishes the authoritative final state.
                                g_authorization_request_in_flight = false;
                            }
                            if (error) {
                                const char* detail =
                                    [[error localizedDescription] UTF8String];
                                LOG_WARN(std::string(
                                             "[notifications] macOS "
                                             "authorization request failed") +
                                         (detail
                                              ? ": " + std::string(detail)
                                              : ""));
                            }
                            UNUserNotificationCenter* center =
                                [UNUserNotificationCenter
                                    currentNotificationCenter];
                            refresh_authorization_settings(
                                center, generation);
                        }];
    }

    notification_detail::publish_authorization_state({
        NotificationAuthorizationStatus::Requesting, false, true});
    return true;
}

bool show(const NotifyPayload& payload) {
    const auto state = notification_authorization_state();
    if (state.status == NotificationAuthorizationStatus::Authorized ||
        state.status == NotificationAuthorizationStatus::Provisional) {
        return submit_authorized_notification(payload);
    }
    if (state.status == NotificationAuthorizationStatus::Denied ||
        state.status == NotificationAuthorizationStatus::Unavailable) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mac_mu);
        if (!g_initialized) return false;
        if (!g_pending_payload.has_value()) g_pending_payload = payload;
    }
    if (state.status == NotificationAuthorizationStatus::Requesting) {
        return true;
    }
    return request_authorization();
}

void shutdown() {
    UNUserNotificationCenter* center = nil;
    ACECodeNotificationCenterDelegate* delegate = nil;
    {
        std::lock_guard<std::mutex> lock(g_mac_mu);
        center = g_center;
        delegate = g_delegate;
        g_center = nil;
        g_delegate = nil;
        g_initialized = false;
        g_authorization_request_in_flight = false;
        g_pending_payload.reset();
        ++g_generation;
    }

    if (center) {
        center.delegate = nil;
        [center removeAllPendingNotificationRequests];
        [center removeAllDeliveredNotifications];
    }
    [delegate release];
}

bool open_settings() {
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:
            @"x-apple.systempreferences:com.apple.Notifications-Settings.extension"];
        return url && [[NSWorkspace sharedWorkspace] openURL:url] == YES;
    }
}

void* capture_tui_window() {
    return nullptr;
}

bool window_is_foreground(void* native_window) {
    NSWindow* window = static_cast<NSWindow*>(native_window);
    return window && [NSApp isActive] == YES && [window isKeyWindow] == YES;
}

bool activate_window(void* native_window) {
    NSWindow* window = static_cast<NSWindow*>(native_window);
    if (!window) return false;

    void (^activate)(void) = ^{
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if ([window isMiniaturized]) [window deminiaturize:nil];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    };
    if ([NSThread isMainThread]) {
        activate();
    } else {
        dispatch_async(dispatch_get_main_queue(), activate);
    }
    return true;
}

void handle_foreground_notification(
    UNNotification* /*notification*/,
    void (^completion_handler)(UNNotificationPresentationOptions options)) {
    completion_handler(
        UNNotificationPresentationOptionBanner |
        UNNotificationPresentationOptionList |
        UNNotificationPresentationOptionSound);
}

void handle_notification_response(UNNotificationResponse* response) {
    if (!response ||
        [response.actionIdentifier
            isEqualToString:UNNotificationDismissActionIdentifier]) {
        return;
    }
    NotifyPayload payload = payload_from_notification(response.notification);
    if (payload.title.empty() && payload.body.empty()) return;

    dispatch_async(dispatch_get_main_queue(), ^{
        dispatch_notification_activation(payload);
    });
}

} // namespace acecode::desktop::notification_backend

#endif // __APPLE__
