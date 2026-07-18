## Context

The current Web UI already emits structured notification payloads for permission requests, questions, and normally completed turns. It also applies the configured focus/session suppression rules before calling the Desktop bridge. On Windows, the bridge delegates to a shared WinToast implementation used by both Desktop and TUI, and each toast owns the payload required to restore its exact session. On macOS the same native API is currently compiled as a no-op.

`ACECode.app` is a real application bundle with the stable identifier `dev.acecode.desktop`, a minimum deployment target of macOS 11, an AppKit event loop, and an existing window/session focus path. The UserNotifications framework is therefore available without a compatibility fallback. Authorization and delivery are asynchronous, and the notification-center delegate must outlive all delivered notifications.

## Goals / Non-Goals

**Goals:**

- Deliver macOS Desktop notifications for permission, question, and normal completion events.
- Preserve the existing Web payload, filtering, and current/cross-workspace activation behavior.
- Keep each delivered notification independently routable.
- Expose the macOS authorization state and an explicit authorization action in Settings.
- Keep notification failures and permission denial non-fatal.
- Preserve Windows WinToast and standalone TUI behavior.

**Non-Goals:**

- Native notifications from the standalone macOS TUI.
- APNs, remote push, critical alerts, badges, or custom notification actions.
- Relaunch-and-resume after `ACECode.app` has fully exited.
- Changing notification event semantics or the persisted notification configuration schema.

## Decisions

### 1. Split the portable notification facade from platform backends

Rename the public boundary to `notifications.hpp` and move payload parsing, completion payload construction, callback registration, lifecycle coordination, authorization-state naming, and activation dispatch into a portable `notifications.cpp`.

Select exactly one backend at build time:

- `notifications_win.cpp` keeps WinToast delivery and Windows window helpers.
- `notifications_mac.mm` uses AppKit and UserNotifications.
- `notifications_stub.cpp` retains no-op behavior elsewhere.

The public initialization options use UTF-8 application metadata plus an opaque native window. The Windows backend converts these values for WinToast, so callers and tests no longer expose wide-string/AUMI-specific types in the common contract.

### 2. Use `UNUserNotificationCenter` local notifications

The macOS backend installs a retained Objective-C delegate, queries authorization settings, and submits immediate `UNNotificationRequest` objects. Request identifiers derive from the payload id plus a process-local sequence so repeated events cannot replace each other; the complete id/workspace/session/title/body tuple is copied into `userInfo`.

The delegate presents banner/list/sound while ACECode is foregrounded and dispatches the exact payload from the selected notification on the default action. The existing Desktop `focus_session` callback remains responsible for restoring the window and selecting the same- or cross-workspace session.

AppleScript and deprecated AppKit notification APIs were rejected because they do not provide the stable application identity and per-notification activation contract required here.

### 3. Make authorization state asynchronous but observable

The facade exposes `unknown`, `not_determined`, `requesting`, `denied`, `authorized`, `provisional`, and `unavailable` states plus an authorization-change callback. Desktop binds native query/request/open-settings functions and forwards native state changes to the Web UI through a DOM custom event.

The Settings notification card queries this state on macOS. Native queries asynchronously refresh `UNNotificationSettings`, and Settings queries again when the app regains focus so changes made in System Settings are observed without restarting. Enabling notifications requests authorization when needed; denied state remains visibly distinct and offers an “Open System Settings” action. Windows and browser-only sessions keep their current UI behavior.

When the first qualifying event arrives before a choice has been made, the macOS backend retains a bounded pending payload and requests authorization. If granted, it delivers that payload; if denied, it drops it. This avoids an unsolicited launch-time prompt without losing the first meaningful alert.

### 4. Keep activation and shutdown race-safe

The portable facade copies callbacks under a mutex and invokes them after releasing the lock. The macOS backend uses a lifecycle generation to ignore authorization completions from an earlier initialization. Shutdown clears callbacks first, detaches/releases the delegate, clears pending requests and delivered notifications, then invalidates backend state.

All AppKit foreground work is dispatched to the main queue. WebHost evaluation already dispatches to the WebView loop, so notification delegate callbacks never mutate UI state directly from an arbitrary system callback queue.

### 5. Preserve current release identity

The backend relies on the existing `dev.acecode.desktop` bundle identifier and bundled icon. CMake adds the Objective-C++ source and links `UserNotifications.framework`; no vcpkg or APNs entitlement is added. Signed release packages are included in manual verification because macOS persists notification permission against application identity.

## Risks / Trade-offs

- [The user denies authorization] → Record the denied state, keep the application usable, and expose recovery guidance in Settings.
- [The authorization callback races with shutdown] → Guard callbacks with an initialization generation and clear facade handlers before backend teardown.
- [A foreground notification is suppressed by macOS defaults] → Implement the foreground delegate method and explicitly request banner/list/sound presentation.
- [Several notifications are outstanding] → Store routing fields in each request’s `userInfo` instead of a global “last payload”.
- [System Settings URLs vary across macOS releases] → Treat opening Settings as best-effort and keep textual recovery guidance when it fails.
- [Refactoring the Windows boundary regresses WinToast] → Keep backend behavior unchanged and retain focused Windows compile/runtime smoke coverage.

## Migration Plan

1. Introduce the portable facade and platform backend selection while keeping Windows behavior green.
2. Add the macOS backend and Desktop lifecycle integration.
3. Add authorization bridges and Settings state.
4. Run portable, Web, macOS build, and opt-in runtime smoke checks.

Rollback is a source revert. No persisted configuration or session migration is introduced.

## Open Questions

None.
