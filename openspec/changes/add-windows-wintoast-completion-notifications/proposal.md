## Why

ACECode on Windows currently uses a Desktop-only `Shell_NotifyIcon` balloon, so completion alerts depend on the tray icon, do not use the modern Windows notification surface, and are unavailable in the TUI. Users need a completion notification from either Windows surface and a reliable way to return directly to the session that finished.

## What Changes

- Add WinToast as a Windows-only dependency and replace the legacy tray-balloon notification backend with a shared WinToast backend.
- Emit a task-completion notification when a normal assistant turn finishes in both `acecode.exe` (TUI) and `acecode-desktop.exe`.
- Carry the workspace/session identity in every notification activation handler.
- When a completion notification is clicked, restore/foreground the owning window and navigate or resume to the completed session.
- Keep Desktop's existing question notifications and focus/config suppression behavior while decoupling native notifications from tray initialization.
- Make the Desktop native bridge accept the payload shape actually sent by the Web UI, including the existing JSON-string compatibility form.
- Preserve non-Windows behavior as a no-op and keep notification failures non-fatal.

## Capabilities

### New Capabilities

- `windows-session-notifications`: Windows-native WinToast completion notifications, activation routing, and equivalent TUI/Desktop behavior.

### Modified Capabilities

None.

## Impact

- Build/dependencies: `vcpkg.json`, root CMake target wiring, Windows system libraries through the WinToast port.
- Native notification boundary: shared Windows notification implementation currently located under `src/desktop/notifications_win.*`.
- Desktop: native startup/teardown, WebView notification bridge, tray notification coupling, and existing session-focus routing.
- TUI: completion transition handling, native window activation, and direct resume-by-session-id support.
- Tests/docs: focused native payload/activation tests, TUI routing tests where practical, existing Web notification tests, and build verification for both executables.
