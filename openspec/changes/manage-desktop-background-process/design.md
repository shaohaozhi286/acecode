## Context

Desktop uses one shared daemon runtime directory and a `DaemonPool` slot to host all workspaces. Today `activate()` spawns first and only reads the old port/token after the spawn fails. That fallback does not attach the old PID to a supervisor, so the next real app quit cannot stop it. POSIX supervision also assumes every tracked PID is a direct child, while macOS has no Windows Job Object equivalent.

The daemon owns sessions and interactive prompts independently of WebSocket clients. Permissions currently have a passive five-minute deny timeout, and pending questions are not replayed when a later UI subscribes. This makes a temporarily headless daemon behave differently from a connected daemon.

The macOS WebView already supports hiding/showing its native window, and the menu-bar item can foreground it. The main process currently installs close-to-tray behavior only for Windows, changes the macOS activation policy to an accessory app when hidden, and has no Dock-reopen hook.

## Goals / Non-Goals

**Goals:**

- Reuse a healthy compatible Desktop-managed daemon as the primary startup path.
- Distinguish spawned and attached processes and make stop-versus-keep behavior explicit.
- Keep the default coupled lifecycle while allowing the user to opt into daemon survival after a real ACECode quit.
- Make replacement and cleanup identity-safe, including rapid quit/relaunch and late teardown.
- Preserve unanswered interactions across periods without a Desktop connection.
- Make macOS window close hide only, with Dock and menu-bar restoration.

**Non-Goals:**

- Starting ACECode or its daemon at login.
- Surviving reboot, providing launchd/systemd installation, or auto-restarting a crashed daemon.
- Connecting to, stopping, upgrading, or garbage-collecting standalone CLI daemons.
- Changing an explicitly configured AskUserQuestion deny/timeout policy.

## Decisions

### 1. Desktop-managed identity is explicit and generation-bound

Desktop launches the daemon with an internal managed-mode flag, a generated daemon GUID, and a desktop protocol version. The daemon writes a managed runtime manifest containing its PID, GUID, protocol, and ACECode version alongside the existing heartbeat/port/token files. Health reports the same managed identity.

The shared Desktop run directory remains reserved for the Desktop-managed process. Runtime cleanup removes files only when the on-disk PID and GUID still match the exiting generation. This prevents a late old process from deleting a replacement's files.

An existing process is eligible for destructive replacement only after its PID, GUID, heartbeat, executable identity, reserved runtime location, and managed identity agree. A normal CLI daemon in the default run directory is never a candidate. A one-time legacy path recognizes the pre-manifest daemon in the reserved Desktop directory so the first protocol upgrade cannot deadlock behind the old release.

Alternative considered: infer ownership only from `daemon.pid`. Rejected because PID reuse and user-started daemons make PID-only killing unsafe.

### 2. Startup is connect-first, with protocol compatibility

`DaemonPool::activate()` inspects the runtime bundle before allocating a port. It verifies the live health response against the runtime PID/GUID/port and accepts an existing daemon when it declares the current Desktop protocol. ACECode application versions may differ when the protocol matches.

If the process is verified as Desktop-managed but the protocol is absent or incompatible, Desktop stops its whole managed process group, waits for termination, removes only the matching runtime generation, and spawns a new random-port daemon without prompting. If identity cannot be established, startup fails safely instead of killing an unknown process.

Alternative considered: always spawn and reclaim on failure. Rejected because it creates avoidable overlap and cannot supervise the reclaimed process.

### 3. Supervision separates connection source from exit policy

Each pool slot records whether the process was `spawned` or `attached`. `IDaemonSupervisor` supports attaching to a verified PID and releasing ownership without termination. Real app shutdown calls a policy-aware pool shutdown:

- `stop_with_desktop`: stop the managed process group, regardless of whether it was spawned or attached.
- `keep_alive`: release local handles/Job ownership and leave the daemon running.

Changing the preference updates the active supervisor policy but does not terminate a running process. The new value is applied on the next real app quit.

On Windows, the Job Object kill-on-close flag follows the policy. On POSIX, a Desktop owner record lets a managed daemon detect loss of its owning Desktop; when the preference is off it exits after a short handoff window, and when on it remains available for attachment. A newer owner record wins during rapid relaunch.

Alternative considered: destroy or disable `DaemonSupervisor` when persistence is enabled. Rejected because health tracking, explicit shutdown, upgrade replacement, and child reaping are still required.

### 4. The persistence preference is global and Desktop-native

`desktop.continue_background_process` is a sparse, global config boolean with default `false`. The Settings UI talks to a native bridge so changing it also updates the live `DaemonPool`; browser-only Web UI does not render a nonfunctional control. The exact user-facing switch text is “退出 ACECode 后继续运行后台进程”.

The preference describes a real application quit (Cmd-Q, tray/menu “退出”, upgrade restart, or native quit bridge), not closing the window.

### 5. macOS close and quit are separate paths

The macOS close request handler always consumes the window close and orders the window out without changing the app into accessory-only mode. A macOS application-activation/reopen observer shows and foregrounds the window when the Dock icon is selected. The existing menu-bar show action uses the same foreground path.

Explicit quit bypasses the close handler, terminates the WebView loop, tears down native UI resources, then applies the daemon exit policy.

### 6. Interactive requests outlive UI connections

The daemon-side permission prompter has no passive timeout by default; abort, permission-mode changes, shutdown, or the first valid response still resolve it. Explicit timeout construction remains supported for tests or deliberate callers.

Both permission and AskUserQuestion prompters expose snapshots of unresolved requests. A WebSocket subscribe acknowledges the session and then replays both snapshot types without sequence numbers, allowing Desktop or a future channel to answer. Response handling remains request-ID based and first-response-wins.

## Risks / Trade-offs

- **[Legacy process classification could be too broad]** → Limit it to the reserved Desktop runtime directory plus matching executable, runtime identity, health PID/GUID, and expected Desktop launch characteristics; remove the legacy path after migration is no longer needed.
- **[A daemon can exit while a new Desktop is launching]** → Write the new owner record immediately after acquiring the single-instance lock and use an instance ID plus a short handoff window before coupled POSIX termination.
- **[Attached POSIX processes are not waitable children]** → Use liveness polling after TERM and escalate to the verified process group only after a grace period.
- **[Config may change while Desktop is open]** → Reload the latest config inside the native setter, change only the Desktop preference, atomically save, and update the in-memory exit policy after persistence succeeds.
- **[Indefinite prompts can retain a worker indefinitely]** → This is intentional for transport continuity; explicit abort, session shutdown, and permission-mode transitions remain escape hatches.

## Migration Plan

1. Ship protocol/manifest support and connect-first discovery together.
2. On first launch, classify an existing pre-manifest process in the reserved Desktop directory as a legacy managed generation, stop it silently, and start a protocol-aware generation.
3. Existing configs omit the new field and therefore retain the safe default (`false`).
4. Rollback leaves an ordinary managed daemon in the reserved directory; the previous Desktop may reclaim it through its existing fallback, while a normal quit still stops it when possible.

## Open Questions

None. Product choices for default state, copy, headless interaction behavior, incompatible upgrades, and macOS close behavior are confirmed.
