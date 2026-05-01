# add-desktop-shell — Design

> **Reads**: `proposal.md` (this change), `openspec/changes/add-web-daemon/`,
> `openspec/changes/add-web-chat-ui/`.
>
> **Touches**: new `src/desktop/`, modifies `src/cli.cpp`, `CMakeLists.txt`,
> `src/utils/paths.{hpp,cpp}` (no new path roots — reuses `User` mode), and
> a small extension to `src/web/auth.cpp` to support per-launch loopback
> tokens.

---

## 1. Architecture overview

```
                  ┌──────────────────────────────────┐
                  │  acecode-desktop  (native shell) │
                  │                                  │
                  │  ┌────────────┐  ┌────────────┐ │
                  │  │ DaemonSup  │  │  WebView   │ │
                  │  │  (spawn,   │  │  (loads    │ │
                  │  │   watch,   │  │   http:// …│ │
                  │  │   stop)    │  │   /?t=…)   │ │
                  │  └─────┬──────┘  └─────┬──────┘ │
                  │        │ stdout/IPC    │ JS↔C++ │
                  │        │               │ bridge │
                  └────────┼───────────────┼────────┘
                           ▼               ▼
                  ┌────────────────────────────┐
                  │  acecode daemon (subprocess)│
                  │  Crow on 127.0.0.1:<rand>   │
                  │  serves /, /api, /ws/...    │
                  └────────────────────────────┘
```

The desktop binary is the **parent** process and the daemon is its child.
This keeps the well-tested `worker.cpp::run_worker` path untouched and lets
the desktop process die without leaking the daemon (we set
`PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` + Job Object on Windows, `prctl
PR_SET_PDEATHSIG` on Linux, and `posix_spawn` + signal-on-exit cleanup on
macOS — see §4 below).

---

## 2. Decision: native shell library

**Decision**: use [`webview/webview`](https://github.com/webview/webview)
(MIT, single-header-style C/C++).

**Rationale**:
- Maps directly onto OS-native WebViews:
  - Windows: WebView2 (Edge Chromium)
  - macOS: WKWebView
  - Linux: WebKitGTK
- C++ API surface fits this codebase: `webview::webview w(...); w.navigate(...);
  w.bind("name", handler); w.run();`
- Single dependency, ships as a vcpkg port (`webview2` on Windows is a NuGet
  package — vcpkg has overlay support).
- No new toolchain.

**Alternatives considered**:
- **Direct WebView2 SDK on Windows** — too much per-platform glue.
- **wxWebView** — pulls all of wxWidgets for one widget.
- **Sciter** — license complexity (commercial licensing for some uses).

**Risk**: `webview/webview` is small and could go unmaintained. Mitigation:
the API surface we use is tiny (~6 entry points), and we wrap it behind
`src/desktop/web_host.hpp` so the implementation can be swapped without
touching the supervision layer.

---

## 3. Decision: daemon as child process (not in-process)

**Decision**: the desktop binary spawns `acecode daemon --foreground` and
treats it as a managed subprocess.

**Rationale**:
- Crash isolation. Agent loops do streaming HTTP, run user `bash`
  commands, and call MCP servers — any of which can wedge a process. We
  don't want a wedged tool call to also wedge the WebView and prevent a
  user from cancelling.
- Reuses `daemon/worker.cpp::run_worker` verbatim. Zero new code paths in
  the daemon.
- Makes "attach to an already running daemon" trivial — see §5.
- The TUI / browser / desktop all become symmetric clients of one daemon.

**Tradeoff**: one extra process and a small startup latency (~150 ms on
warm SSD, dominated by Crow init). Acceptable.

**Alternative**: link the agent loop directly into the GUI process. Saves
one process but loses crash isolation and forces us to fork the
`run_worker` startup sequence.

---

## 4. Process supervision details

`src/desktop/daemon_supervisor.{hpp,cpp}` owns the daemon child. Lifecycle:

### 4.1 Spawn
- Compute daemon binary path: same directory as the desktop binary, named
  `acecode` (POSIX) / `acecode.exe` (Windows). If absent, fall back to
  whatever `acecode` resolves to via `$PATH`.
- Build argv:
  ```
  acecode daemon --foreground --port 0 --emit-runtime-json
  ```
  - `--port 0` is **new** (today's daemon hard-defaults to `web.port`). We
    extend `daemon/cli.cpp` to accept `--port <N>` and treat `0` as
    "ephemeral, OS picks". See §7.
  - `--emit-runtime-json` is **new**: instead of the current human banner
    "Daemon listening on …", emit a single JSON line on stdout with
    `{"port": …, "token": …, "guid": …, "data_dir": …}` once the server is
    bound. The desktop reads this line, then continues to consume stdout
    for log capture. See §7.

### 4.2 Watch
- Read stdout line-by-line on a worker thread. The first line that parses
  as `{"port": …}` unblocks the WebView load.
- Subsequent lines are appended to a ring buffer (1024 entries) exposed via
  `Help → Show Daemon Logs` in the menu. We do not aggregate to a file from
  the desktop side — the daemon already logs to `<data_dir>/logs/`.
- Heartbeat: existing `<data_dir>/run/heartbeat` JSON, polled every 5 s. If
  `now - timestamp_ms > 15 s` and the child PID is still alive, surface a
  yellow banner "Daemon unresponsive" but don't kill — the user may be in
  the middle of a long bash run.

### 4.3 Cleanup
- On clean quit (window close + tray off, `cmd+q`, `Quit` menu), send
  SIGTERM (POSIX) or `CTRL_BREAK_EVENT` to the process group (Windows).
  Wait up to 5 s, then SIGKILL.
- On crash of the desktop process: rely on OS-level parent-death tracking
  so the daemon dies with us:
  - **Linux**: child calls `prctl(PR_SET_PDEATHSIG, SIGTERM)` immediately
    after fork, before exec. Implementation note: this requires the daemon
    to opt in via a flag, since `prctl` must run in the child between fork
    and exec — we add `--die-with-parent` to the daemon CLI.
  - **macOS**: no clean equivalent. Use a `kqueue` `EVFILT_PROC` watcher in
    the daemon (gated by `--die-with-parent`) that exits when the parent
    PID disappears. Fallback: heartbeat from desktop → daemon every 2 s on
    a unix socket; daemon exits if 3 missed.
  - **Windows**: assign the daemon to a Job Object with
    `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` from the desktop side; no daemon
    cooperation needed.

### 4.4 Restart policy
- v1: no auto-restart. If the daemon dies, the WebView shows a connection
  banner ("Daemon stopped") with two buttons: `Restart` and `View logs`.
- Rationale: an auto-restart loop on a misconfigured daemon (bad provider,
  port bind failure with `--port 0` should not happen but safety) creates
  noise.

---

## 5. Decision: reuse-running-daemon mode

**Decision**: `acecode desktop --reuse-running-daemon` skips spawn and
discovers via the existing `<data_dir>/run/` files (`port`, `token`, `guid`,
`heartbeat`).

**Rationale**:
- Power users running `acecode daemon start` (Service mode) will want the
  GUI to attach instead of spawning a second daemon.
- Existing single-instance protections (GUID mutex in
  `validate_can_start`) already prevent two daemons on the same data dir.

**Implementation**:
- Read `heartbeat`. If `timestamp_ms` is fresh (< 15 s), reuse.
- Else, treat as stale and spawn our own (but don't delete the files —
  `validate_can_start` will GC them).

**Default behavior**: without `--reuse-running-daemon`, the desktop always
spawns its own ephemeral child to keep the lifecycle simple.

---

## 6. Decision: per-launch loopback token

**Decision**: even on loopback, the desktop launch generates a fresh token
and passes it to the WebView via the initial URL `?token=…`. The token is
stored in `<data_dir>/run/token` (mode 0600) for the daemon to validate.

**Rationale**: today the daemon trusts loopback unconditionally
(`auth.cpp::require_auth`). That's fine when the user explicitly browses
to `localhost:28080`, but for desktop the WebView ends up on a port that
any other local process can also probe. The cost of always requiring a
token on the desktop launch path is one extra header — negligible —
versus the cost of a curious local process hitting `/api/sessions`.

**Implementation**:
- Add `web.require_token_on_loopback: bool` (default `false` to preserve
  existing browser-based UX).
- The daemon's `--token-only-mode` CLI flag forces this on for the launch,
  irrespective of config.
- Desktop always passes `--token-only-mode`.
- WebView injects `X-ACECode-Token` for `fetch()` calls via a small
  `web/api.js` change reading `window.__ACECODE_TOKEN__`, set in the
  `index.html` template substitution. Today the token is appended only as
  a query string, which works for WS but isn't sent on every fetch — we
  formalize the injection.

**Not in scope**: TLS for loopback. We continue to rely on loopback being
a trust boundary for transport; the token covers same-host process
isolation.

---

## 7. Daemon CLI additions

| Flag | Effect | Where |
|---|---|---|
| `--port <N>` | Override `cfg.web.port`. `0` = OS picks. | `daemon/cli.cpp` |
| `--emit-runtime-json` | Emit a single JSON line on stdout once bound, then continue normal stdout. | `daemon/worker.cpp` |
| `--die-with-parent` | POSIX: `prctl(PR_SET_PDEATHSIG, SIGTERM)` (Linux only); macOS `kqueue EVFILT_PROC`; Windows: no-op (Job Object handles it from parent side). | `daemon/worker.cpp` |
| `--token-only-mode` | Equivalent to `web.require_token_on_loopback = true` for this launch. | `daemon/cli.cpp` |

All four are additive; no existing daemon invocations change behavior.

---

## 8. Native integrations

### 8.1 System tray

- `src/desktop/tray.{hpp,cpp}` — uses platform APIs:
  - Windows: `Shell_NotifyIcon` with custom HWND
  - macOS: `NSStatusItem`
  - Linux: `libappindicator` (Ubuntu) with fallback to no-op (KDE/GNOME
    parity is famously bad — we don't promise it)
- Menu: `Show Window`, `New Session`, `Pause All Tools`, `Quit`.
- "Close to tray" preference (default ON on Windows/Linux, OFF on macOS
  matching platform conventions).

### 8.2 Native menu

- macOS: full menu bar (`File`, `Edit`, `View`, `Session`, `Window`,
  `Help`).
- Windows: optional menu bar (default off — we let the WebView own the
  chrome) with the same actions exposed via tray + keyboard.
- Linux: same as Windows.

### 8.3 Notifications

- `src/desktop/notify.hpp` — single function `notify(title, body)`.
  - Windows: `ToastNotificationManager` (WinRT)
  - macOS: `NSUserNotificationCenter` (or `UNUserNotificationCenter` if we
    drop 10.13)
  - Linux: `libnotify`
- Triggered from JS via `window.aceDesktop.notify(title, body)` bound by
  `webview::bind`.

### 8.4 File pickers & drag-drop

- File picker (`window.aceDesktop.pickFile({mode:'open'|'save'|'dir'})`)
  exposes platform native dialogs. The chat UI uses this to populate
  arguments for `file_read_tool` etc.
- Drag-drop: WebView already accepts HTML5 dragenter/drop events. The C++
  side intercepts the OS-level drop on Windows (necessary because WebView2
  can't get the file path on its own — only a virtual stream) and binds
  `window.aceDesktop.onFileDrop = (paths) => …`. macOS WKWebView and
  WebKitGTK both expose paths natively.

### 8.5 Single-instance lock

- A separate GUID from the daemon's. Stored at `<data_dir>/run/desktop.lock`.
- Second launch: send a `--show-window` IPC to the running instance
  (Windows: named pipe, POSIX: unix socket at `<data_dir>/run/desktop.sock`)
  and exit 0.

### 8.6 Deep links (`acecode://`)

- Out of scope for v1. Stub the protocol handler registration so we can
  ship it later without re-touching installer logic.

---

## 9. Build / packaging

### 9.1 CMake

Add target in top-level `CMakeLists.txt`:

```cmake
if(ACECODE_BUILD_DESKTOP)
  add_executable(acecode-desktop
    src/desktop/main.cpp
    src/desktop/daemon_supervisor.cpp
    src/desktop/web_host.cpp
    src/desktop/tray_${PLATFORM}.cpp
    src/desktop/notify_${PLATFORM}.cpp
    src/desktop/single_instance_${PLATFORM}.cpp
  )
  target_link_libraries(acecode-desktop PRIVATE
    acecode_testable     # path utilities, runtime files, logger
    webview::webview
  )
endif()
```

`ACECODE_BUILD_DESKTOP` defaults `OFF` so the existing CI matrix (TUI +
daemon) is unchanged. Desktop CI is a separate workflow added in step 9
of `tasks.md`.

### 9.2 vcpkg

- Add `webview` to `vcpkg.json` under a feature `desktop`.
- Windows: WebView2 runtime SDK is downloaded by the `webview` port
  itself. The end-user *runtime* is bootstrapped — see §9.4.
- Linux: depend on system `webkit2gtk-4.1-dev`. Document in README.
- macOS: zero extra deps (WKWebView is in the SDK).

### 9.3 Bundle layout

```
acecode-desktop                        ← the GUI binary
acecode (or acecode.exe)               ← the same daemon binary as today
share/acecode/models_dev/api.json
share/acecode/web/                     ← embedded; daemon already serves it
                                         (no need to copy unless web.static_dir is used)
```

### 9.4 Per-OS packaging

- **Windows**: standalone `.exe` (acecode-desktop) + colocated `acecode.exe`
  + WebView2 Evergreen Bootstrapper (auto-installs runtime if missing).
  Future: MSIX. NSIS for v1.
- **macOS**: `.app` bundle, codesigned + notarized in CI. Universal
  binary (x86_64 + arm64).
- **Linux**: AppImage embedding `webkit2gtk-4.1` is too large; we ship a
  `.tar.gz` + `.deb` listing `libwebkit2gtk-4.1-0` as a runtime dep. Flatpak
  is a follow-up.

### 9.5 Dev iteration

While developing the front-end, devs run `acecode daemon --foreground` in
one terminal and open the URL in a regular browser — same as today. The
desktop binary is only needed to test desktop-specific behaviors (tray,
notifications, drag-drop). Document this in `docs/desktop-dev.md`.

---

## 10. Front-end deltas

Goal: zero breaking changes to existing `web/` components. The browser UI
and the desktop UI render the same DOM.

Additive only:

- `web/api.js` — thin platform-detection helper:
  ```js
  export const isDesktop = !!window.aceDesktop;
  ```
- `window.aceDesktop` (only present in desktop builds) exposes:
  - `notify(title, body)`
  - `pickFile(opts)`
  - `openExternal(url)` — open links in OS browser instead of inside
    WebView (critical so HTTP links from chat output don't replace the SPA)
  - `setBadge(n)` — Dock/taskbar badge (macOS + Windows)
  - `onFileDrop` — assign a callback
- `index.html` template adds:
  ```html
  <script>window.__ACECODE_TOKEN__ = "{{TOKEN}}";</script>
  ```
  populated by the daemon when serving `/index.html` to a request that has
  `web.require_token_on_loopback` enabled.
- `connection.js` — read `window.__ACECODE_TOKEN__` and inject as
  `X-ACECode-Token` on every `fetch`.

The components themselves (`ace-app`, `ace-chat`, …) are untouched.

---

## 11. Testing strategy

Unit-testable in `acecode_testable` (no FTXUI, no WebView):

- `src/desktop/runtime_files.{hpp,cpp}` — wrappers around
  `<data_dir>/run/` reads. Already exists for the daemon side; add a
  symmetric `read_runtime_state()` returning a parsed struct.
  → `tests/desktop/runtime_files_test.cpp`
- `src/desktop/runtime_json_parser.{hpp,cpp}` — parses a single
  `{"port":…,"token":…}` line. Pure function.
  → `tests/desktop/runtime_json_parser_test.cpp`
- `src/desktop/url_builder.{hpp,cpp}` — composes
  `http://127.0.0.1:<port>/?t=<token>` with proper escaping.
  → `tests/desktop/url_builder_test.cpp`

Not unit-testable (manual / integration matrix):

- Spawn / SIGTERM / job-object — manual smoke per OS.
- Tray / notifications — manual smoke per OS.
- Drag-drop — manual smoke per OS.

CI matrix additions: macOS ARM, Windows x64 — both build the desktop
target and run the unit tests but do not exercise the GUI.

---

## 12. Open questions

These are deferred but worth flagging:

1. **Multi-window**. Per-session windows or single-window-with-sidebar?
   v1 picks single-window; if users push for multi-window, the bridge in
   §10 already supports it via `window.aceDesktop.openWindow(sessionId)`.
2. **Settings UI**. Today config is JSON-edited. Should desktop expose a
   GUI? Recommend deferring to a follow-up change `add-settings-ui`,
   shared across browser and desktop.
3. **Plugin / extension API**. WebView's `bind` mechanism could host
   third-party extensions. Out of scope; flag the surface so we don't
   paint into a corner.
4. **Auto-update**. Sparkle (macOS), Squirrel (Windows), AppImage update.
   Separate change.
5. **Telemetry**. None today. If we add any, the desktop is a natural
   surface for opt-in. Out of scope.

---

## 13. Migration / rollback

- **Migration**: none. New target, opt-in via `ACECODE_BUILD_DESKTOP`. No
  config schema changes (only additive: `web.require_token_on_loopback`).
- **Rollback**: drop the `acecode-desktop` binary. The daemon CLI flags
  added in §7 are additive; leaving them in is harmless. Revert
  `web.require_token_on_loopback` flag if undesired.

---

## 14. Summary of decisions

| # | Decision | Why |
|---|---|---|
| D1 | `webview/webview` library | C++-native, no Rust/Node toolchain, system WebView. |
| D2 | Daemon as child process | Crash isolation, reuses `run_worker`, symmetric with browser/TUI. |
| D3 | `--reuse-running-daemon` opt-in | Power users with system-installed services. |
| D4 | Per-launch loopback token | Defense against same-host snooping; cheap. |
| D5 | `--port 0` ephemeral by default | Avoid 28080 collision in desktop deployment. |
| D6 | Single-window v1 | Avoid window-management bugs; multi-window later. |
| D7 | Native menus + tray + notifications | The whole point of "desktop" vs "tab". |
| D8 | Drop daemon when desktop crashes (OS-level) | No zombie daemons. |
| D9 | Additive front-end only | Browser UI must keep working. |
| D10 | `ACECODE_BUILD_DESKTOP=OFF` by default | Keep current CI green from day one. |
