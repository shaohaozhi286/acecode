#import <AppKit/AppKit.h>

#include "desktop/notifications.hpp"

#include <atomic>
#include <iostream>

using namespace acecode::desktop;

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 520, 240)
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [window setTitle:@"ACECode macOS notification smoke"];
        [window center];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        NotificationInitOptions options;
        options.app_name = "ACECode Notification Smoke";
        options.activation_window = window;
        if (!init_notifications(options)) {
            std::cerr << "notification initialization failed\n";
            [window release];
            return 2;
        }

        std::atomic<int> exit_code{4};
        std::cout
            << "Allow notifications if prompted, then select the ACECode "
               "smoke notification. The check times out after 30 seconds."
            << std::endl;
        set_notification_authorization_handler(
            [&exit_code](const NotificationAuthorizationState& state) {
                std::cout << "authorization="
                          << notification_authorization_status_name(state.status)
                          << std::endl;
                if (state.status == NotificationAuthorizationStatus::Denied ||
                    state.status ==
                        NotificationAuthorizationStatus::Unavailable) {
                    exit_code.store(3);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [NSApp terminate:nil];
                    });
                }
            });
        set_click_handler([&exit_code](const NotifyPayload& payload) {
            std::cout << "activated session=" << payload.session_id
                      << std::endl;
            exit_code.store(0);
            [NSApp terminate:nil];
        });

        NotifyPayload payload;
        payload.id = "macos-runtime-smoke";
        payload.workspace_hash = "smoke-workspace";
        payload.session_id = "smoke-session";
        payload.title = "ACECode macOS notification smoke";
        payload.body =
            "Select this notification to reactivate the test window.";
        if (!show_notification(payload)) {
            std::cerr << "notification was not accepted\n";
            shutdown_notifications();
            [window release];
            return 3;
        }

        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW,
                          static_cast<int64_t>(30 * NSEC_PER_SEC)),
            dispatch_get_main_queue(), ^{
                std::cerr << "notification activation timed out\n";
                [NSApp terminate:nil];
            });
        [NSApp run];
        shutdown_notifications();
        [window release];
        return exit_code.load();
    }
}
