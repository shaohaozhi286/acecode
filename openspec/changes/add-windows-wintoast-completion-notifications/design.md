## Context

ACECode already has a Web-side notification payload and session-focus contract, but the native Windows implementation is a `Shell_NotifyIcon` balloon tied to the Desktop tray message window. It stores only the latest payload, so an older visible alert can route to the wrong session, and `acecode.exe` has no notification path at all. The Web bridge also expects an object even though the current JavaScript helper passes a JSON string.

WinToast provides modern Win32 toast delivery and a per-toast `IWinToastHandler`. The vcpkg registry exposes it as the Windows-only `wintoast` port and CMake target `unofficial::wintoast::wintoast`. The TUI's agent callbacks run on a worker thread, while FTXUI state and session resume must run on the UI thread. Desktop WebView evaluation is already marshalled by `WebHost::eval`.

## Goals / Non-Goals

**Goals:**

- Use WinToast for Windows notifications from both shipped executables.
- Notify only for genuine, normally completed live turns with assistant output.
- Preserve each alert's exact workspace/session routing data.
- Restore the owning window and navigate to the completed session on activation.
- Preserve Desktop question notifications and existing config/focus suppression.
- Keep all notification failures best-effort and non-fatal.

**Non-Goals:**

- Relaunch an ACECode process after it has exited when an Action Center item is clicked.
- Add notification settings UI or change the current configuration schema.
- Add native notification implementations for macOS or Linux.
- Notify separately for child/subagent completion.

## Decisions

### 1. Consume WinToast through the Windows-only vcpkg dependency

Add `{ "name": "wintoast", "platform": "windows" }` to the manifest, call `find_package(unofficial-wintoast CONFIG REQUIRED)` under `WIN32`, and link `unofficial::wintoast::wintoast` from the shared native support target.

This follows the repository's existing manifest/toolchain flow and avoids checking third-party sources into `external/`. Copying the two WinToast source files was considered, but would duplicate package maintenance and license-update work.

### 2. Turn the existing notification source into a shared native backend

Move `notifications_win.cpp` from Desktop-only support into `acecode_native_bridge_support`, which is already linked by both `acecode` and `acecode-desktop`. Keep a cross-platform header and non-Windows stubs so callers do not spread platform conditionals.

The backend will accept initialization options containing a surface-specific application name/AUMI and an activation HWND. Desktop and TUI use distinct AUMIs so WinToast shortcut registration cannot make the two executables overwrite one another's notification identity.

Each `show_notification` call allocates a WinToast handler holding a copy of the complete payload. Activation first restores/foregrounds the captured native window, then dispatches that handler's payload to the registered application callback. This replaces the legacy single global "last balloon" payload and keeps older notifications routable.

### 3. Initialize Desktop notifications independently from the tray

Desktop initializes WinToast with the WebHost HWND whether or not tray creation succeeds. The Web bridge keeps its existing payload fields and delegates to the shared backend. Its parser accepts both a direct object argument and the currently shipped nested JSON-string form.

The activation callback reuses the existing `focus_session(workspace_hash, session_id)` path. That path already distinguishes current-workspace resume from cross-workspace activation and uses JSON escaping before evaluating Web UI hooks. The legacy `NIN_BALLOONUSERCLICK` coupling is removed from the tray window procedure.

### 4. Marshal TUI delivery and activation through the FTXUI event loop

TUI initializes WinToast after the session manager and command registry exist. The activation HWND is the visible console window when available, with the startup foreground terminal window as the Windows Terminal/ConPTY fallback.

The agent loop exposes its existing persisted turn outcome (`completed`, `error`, or `aborted`) immediately before the terminal busy transition. The final `on_busy_changed` callback detects `was_waiting && !busy`, requires the explicit `completed` outcome plus a non-empty final assistant message, snapshots session id/title/body, and posts `show_notification` to `ScreenInteractive::Post`. This prevents an assistant message emitted before a later tool/provider failure from being mistaken for completion and keeps WinToast calls on the same thread that initialized COM.

On toast activation, the backend restores the captured terminal HWND and the callback posts a resume action to the FTXUI thread. A reusable `resume_session_by_id(CommandContext&, id)` entry point wraps the existing canonical `/resume` implementation so notification routing uses the same provider/model, worktree, transcript, token, goal, and title restoration behavior rather than a partial duplicate.

The TUI compares the captured terminal HWND with `GetForegroundWindow()` for `suppress_when_focused`; if no dependable HWND was captured, it favors delivering the alert.

### 5. Retain current completion semantics and payload text

Desktop continues using the existing `turn_completed` reducer effect. Agent turns now carry their explicit `completed`, `error`, or `aborted` outcome on the terminal `busy_changed`/`done` frames; the reducer only emits the completion effect for `completed` (and retains a legacy error-message fallback). TUI mirrors that rule through the native callback described above. Titles use `已完成 · <session title>` (falling back to `会话`) and bodies use the final assistant text truncated at a Unicode-safe boundary.

Errors, aborts, transcript replay, and teardown do not emit completion alerts. Existing question notification payloads continue through the same backend unchanged.

### 6. Make lifecycle and concurrency explicit

The backend protects its registered callback and lifecycle flags with a mutex. WinToast owns each heap-allocated handler after `showToast`; handlers consult the current callback at activation time, so shutdown clears the callback before clearing outstanding toasts. Both executables call shutdown before destroying objects captured by their handlers.

Focused pure helpers cover payload parsing, text truncation, and activation dispatch. Existing Web tests continue to cover suppression and payload construction; Windows builds provide the authoritative WinToast compile/link check.

## Risks / Trade-offs

- [WinToast shortcut registration can fail under Windows policy restrictions] → Treat initialization as a logged non-fatal failure and leave the coding session usable.
- [Foreground activation can still be constrained by Windows foreground-lock rules] → Activation is initiated by a real toast click; use restore/show plus foreground APIs and retain the existing Desktop focus path.
- [Windows Terminal does not expose the real host through `GetConsoleWindow()`] → Capture the startup foreground window when the console HWND is absent or invisible.
- [A toast may activate concurrently with shutdown] → Clear the application callback before clearing WinToast notifications and marshal all UI mutations to each surface's main loop.
- [Adding a manifest dependency lengthens first Windows configure] → Restrict it to the Windows platform expression; non-Windows dependency graphs remain unchanged.

## Migration Plan

1. Add and configure the Windows-only vcpkg port.
2. Replace the tray-balloon backend while retaining its public payload contract.
3. Decouple Desktop initialization/activation from the tray and verify both object payload forms.
4. Add TUI completion and activation routing through the shared backend.
5. Remove obsolete balloon click handling and stale comments.
6. Run focused tests, the full Web suite/build, CMake configure, both Windows executable builds, and the C++ test suite.

Rollback is a source revert: no persisted session or config migration is introduced.

## Open Questions

None.
