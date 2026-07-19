## 1. Portable Notification Boundary

- [x] 1.1 Introduce the platform-neutral notification header, facade, and internal backend contract.
- [x] 1.2 Move WinToast delivery/window helpers into the Windows backend without changing Windows behavior.
- [x] 1.3 Add the unsupported-platform backend and update callers, CMake, comments, and portable tests.

## 2. macOS Native Backend

- [x] 2.1 Implement UserNotifications initialization, authorization tracking, and bounded first-event retention.
- [x] 2.2 Implement immediate local delivery, foreground presentation, independent payload activation, and AppKit window activation.
- [x] 2.3 Implement race-safe shutdown, delivered-notification cleanup, and best-effort System Settings opening.

## 3. Desktop and Settings Integration

- [x] 3.1 Initialize macOS notifications independently of the tray and preserve the existing Desktop focus-session callback.
- [x] 3.2 Add native authorization query/request/settings bridges and Web authorization helper tests.
- [x] 3.3 Extend the existing Settings notification card with macOS authorization status and recovery actions using repository style tokens.

## 4. Verification

- [x] 4.1 Add focused C++ tests for shared authorization and activation contracts plus a macOS opt-in runtime smoke.
- [x] 4.2 Run focused and complete Web tests and build.
- [x] 4.3 Configure/build the macOS TUI, Desktop, and C++ unit target; run notification-focused and full unit tests.
- [x] 4.4 Validate OpenSpec artifacts and audit every requirement against implementation and verification evidence.
