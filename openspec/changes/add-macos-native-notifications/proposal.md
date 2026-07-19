## Why

ACECode already publishes actionable native notifications on Windows, but the macOS Desktop shell keeps the same notification bridge as a no-op. macOS users need completion, permission, and question alerts that preserve the originating workspace/session and return them to the correct conversation.

## What Changes

- Add a macOS native notification backend using the UserNotifications framework.
- Preserve the existing notification payload, filtering, and exact-session activation behavior across Windows and macOS.
- Make notification authorization explicit in the Desktop settings UI and surface the operating-system authorization state.
- Decouple the shared notification contract and pure helpers from Windows-only backend details.
- Keep failures, denied authorization, and unsupported platforms non-fatal.
- Limit this change to `ACECode.app`; standalone macOS TUI notification delivery remains out of scope.

## Capabilities

### New Capabilities

- `macos-desktop-notifications`: macOS Desktop local-notification authorization, delivery, foreground presentation, and workspace/session activation routing.

### Modified Capabilities

None.

## Impact

- Native notification boundary under `src/desktop/`, including the existing Windows implementation and shared tests.
- Desktop startup, WebView bridge bindings, notification activation, and shutdown in `src/desktop/main.cpp`.
- Settings and desktop bridge helpers under `web/src/`.
- macOS CMake framework linkage and Objective-C++ sources.
- Release/runtime verification for signed Intel and Apple Silicon `ACECode.app` bundles.
