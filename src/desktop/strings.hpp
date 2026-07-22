#pragma once

#include <string>
#include <string_view>

namespace acecode::desktop {

enum class DesktopStringId {
    Unknown,
    TrayPinned,
    TrayRecent,
    TrayMore,
    TrayNewSession,
    TrayOpenApp,
    TrayQuit,
    NewSessionFallback,
    AboutTitle,
    AboutAceCodeVersion,
    AboutBrowserVersion,
    AboutCompilerVersion,
    FolderPickerTitle,
    FolderPickerPrompt,
    FolderPickerMissing,
    StartupFailedTitle,
    StartupUnexpected,
    StartupLogInstruction,
    StartupException,
    DaemonMissing,
    DaemonWorkspaceFailed,
    BrowserFallbackNoDaemon,
    BrowserFallbackOpenFailed,
    ReasonLabel,
    WebViewFailureLabel,
    BrowserFailureLabel,
    NotificationCompleted,
    NotificationSession,
    NotificationBlankTurn,
    Count,
};

// Pure catalog lookup. Invalid locale values preserve the legacy zh-CN copy.
std::string_view desktop_string(DesktopStringId id, const std::string& locale);

// Process-wide native presentation locale. The Desktop main thread updates it
// after config resolution and through the runtime locale bridge.
void set_native_locale(const std::string& locale);
std::string native_locale();
std::string_view native_string(DesktopStringId id);

std::string format_startup_exception_message(const std::string& details,
                                             const std::string& locale);
std::string format_daemon_missing_message(const std::string& locale);
std::string format_daemon_workspace_failed_message(const std::string& workspace,
                                                   const std::string& details,
                                                   const std::string& locale);
std::string format_browser_fallback_no_daemon_message(
    const std::string& reason,
    const std::string& webview_error,
    const std::string& locale);
std::string format_browser_fallback_open_failed_message(
    const std::string& reason,
    const std::string& browser_error,
    const std::string& webview_error,
    const std::string& locale);

} // namespace acecode::desktop
