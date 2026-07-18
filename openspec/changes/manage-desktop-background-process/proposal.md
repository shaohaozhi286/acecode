## Why

ACECode Desktop currently treats its daemon as an opaque child process: a surviving daemon is only reclaimed as a fallback, is not fully supervised afterward, and may be left behind when Desktop exits. On macOS, closing the last window also terminates the app instead of behaving like a normal background-capable Mac app, which prevents reliable background work and reconnectable interactions.

## What Changes

- Make Desktop discover and attach to a healthy, compatible Desktop-managed background process before spawning a replacement.
- Track whether the active process was spawned or attached and apply an explicit exit policy instead of relying on supervisor destruction.
- Add a global Desktop-only setting labeled “退出 ACECode 后继续运行后台进程”, disabled by default.
- Keep the active process running after a real app quit when the setting is enabled; when disabled, stop it on the next real app quit without killing it immediately when the switch is changed.
- Identify Desktop-managed processes explicitly, leave standalone CLI daemons untouched, and safely replace incompatible managed processes without user confirmation.
- Preserve pending permission and question interactions while no Desktop window is connected so a later Desktop or future channel can answer them.
- On macOS, make the window close button hide the window without quitting ACECode, and restore/foreground it from the Dock or menu-bar icon.
- Keep login startup, reboot persistence, and crash auto-restart outside this change.

## Capabilities

### New Capabilities

- `desktop-background-process`: Discovery, ownership, compatibility, supervision, persistence preference, safe replacement, and cleanup of Desktop-managed background processes.
- `desktop-window-lifecycle`: macOS close, Dock reopen, menu-bar reopen, and real application quit behavior.
- `detached-interaction-continuity`: Durable pending interactive requests across periods with no connected Desktop UI.

### Modified Capabilities

None.

## Impact

- Desktop lifecycle and native host code under `src/desktop/`.
- Daemon runtime identity, lifecycle metadata, health/management APIs, and process cleanup under `src/daemon/` and `src/web/`.
- Global Desktop configuration serialization under `src/config/`.
- Settings UI and Desktop bridge helpers under `web/src/`.
- Permission/question prompting and reconnect snapshots under `src/session/` and WebSocket routes.
- Unit, web, lifecycle, and macOS validation coverage, plus daemon API documentation where the protocol changes.
