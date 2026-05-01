# add-desktop-shell — Tasks

> Implementation checklist. Order matters: each phase unblocks the next.
> See `design.md` for the why behind each step.

## Phase 1 — Daemon CLI extensions (no GUI yet)

- [ ] **T1.1** Add `--port <N>` to `daemon/cli.cpp`. Treat `0` as ephemeral.
      Plumb through to `Crow::App::bindaddr().port(N)`. Verify Crow reports the
      bound port (`server.port()`) for `--port 0`.
- [ ] **T1.2** Add `--emit-runtime-json` to `daemon/cli.cpp`. After Crow is
      bound, emit one line of JSON to stdout
      (`{"port":N,"token":"…","guid":"…","data_dir":"…"}`) before any other
      logs.
- [ ] **T1.3** Add `--die-with-parent` to `daemon/cli.cpp`. POSIX impl: Linux
      `prctl(PR_SET_PDEATHSIG, SIGTERM)` in the child between fork/exec
      paths; macOS `kqueue EVFILT_PROC` watcher thread. Windows: no-op
      (Job Object handles it from the desktop side).
- [ ] **T1.4** Add `--token-only-mode` to `daemon/cli.cpp`. Sets the new
      `web.require_token_on_loopback = true` for this launch only.
- [ ] **T1.5** Extend `src/web/auth.cpp::require_auth` to honor
      `cfg.web.require_token_on_loopback`. Default `false` preserves current
      behavior.
- [ ] **T1.6** Update `daemon/runtime_files.cpp` to write the same payload
      that `--emit-runtime-json` emits (no behavior change for service mode).
- [ ] **T1.7** Tests for `--port 0`, `--emit-runtime-json`, and the new
      `require_auth` branch in `tests/daemon/`.

## Phase 2 — Desktop bridge to existing front-end (browser-testable)

- [ ] **T2.1** Add `window.__ACECODE_TOKEN__` injection in
      `web/index.html` template. Daemon substitutes the token at serve time
      when `require_token_on_loopback` is true.
- [ ] **T2.2** Update `web/connection.js` and `web/api.js` to inject the
      `X-ACECode-Token` header from `window.__ACECODE_TOKEN__` on every
      `fetch` and to append `?token=` for WS connects.
- [ ] **T2.3** Add `web/desktop.js` (loaded only when
      `window.aceDesktop` exists) with `notify`, `pickFile`, `openExternal`,
      `setBadge`, `onFileDrop` callable from components.
- [ ] **T2.4** Wire `ace-chat` to call `window.aceDesktop.openExternal(url)`
      for external links instead of the default in-WebView navigation.
- [ ] **T2.5** Smoke test: load the page in a regular browser with
      `window.aceDesktop` undefined → all features still work, no JS errors.

## Phase 3 — Desktop binary skeleton

- [ ] **T3.1** Add `vcpkg.json` feature `desktop` with `webview` dependency.
- [ ] **T3.2** Add `cmake/acecode_desktop.cmake` defining the
      `acecode-desktop` target. Gate behind `ACECODE_BUILD_DESKTOP` (default
      OFF).
- [ ] **T3.3** Create `src/desktop/main.cpp` — minimal: parse argv, log to
      `<data_dir>/logs/desktop.log`, exit.
- [ ] **T3.4** Create `src/desktop/runtime_json_parser.{hpp,cpp}` — pure
      function: parse `{"port":…,"token":…}` line, return struct or error.
- [ ] **T3.5** Create `src/desktop/url_builder.{hpp,cpp}` — pure function:
      compose URL with proper percent-encoding of token.
- [ ] **T3.6** Tests for T3.4 and T3.5 in `tests/desktop/`.

## Phase 4 — Daemon supervision

- [ ] **T4.1** `src/desktop/daemon_supervisor.{hpp,cpp}` — spawn child
      process with stdout pipe. Per-OS process API (`posix_spawn` on POSIX,
      `CreateProcess` on Windows).
- [ ] **T4.2** Reader thread: line-buffer stdout, parse first JSON line
      via T3.4, then push subsequent lines into a 1024-entry ring buffer.
- [ ] **T4.3** Windows-only: assign child to a Job Object with
      `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
- [ ] **T4.4** Clean shutdown: SIGTERM (POSIX) or `CTRL_BREAK_EVENT`
      (Windows), 5s grace, then SIGKILL.
- [ ] **T4.5** Heartbeat poller: every 5s read `<data_dir>/run/heartbeat`,
      surface "Daemon unresponsive" if `now - timestamp_ms > 15s`.
- [ ] **T4.6** Implement `--reuse-running-daemon`: skip spawn if heartbeat
      is fresh. Fall through to spawn if stale.

## Phase 5 — WebView host

- [ ] **T5.1** `src/desktop/web_host.{hpp,cpp}` — wrap `webview::webview`.
      API: `set_url`, `bind(name, fn)`, `run`, `terminate`.
- [ ] **T5.2** On startup: wait for daemon JSON line → compose URL →
      `set_url` → `run`.
- [ ] **T5.3** Bind native bridges: `notify`, `pickFile`, `openExternal`,
      `setBadge`, `onFileDrop`.
- [ ] **T5.4** Window state persistence: remember position, size,
      maximized state in `<data_dir>/desktop_state.json`. Restore on launch.

## Phase 6 — Native integrations (per-OS)

- [ ] **T6.1** Tray icon abstraction `src/desktop/tray.hpp` + per-OS
      implementations (`tray_win.cpp`, `tray_mac.mm`, `tray_linux.cpp`).
- [ ] **T6.2** Native notification abstraction
      `src/desktop/notify.hpp` + per-OS implementations.
- [ ] **T6.3** File picker abstraction `src/desktop/file_picker.hpp` +
      per-OS implementations.
- [ ] **T6.4** Single-instance lock `src/desktop/single_instance.hpp` +
      per-OS implementations (named pipe / unix socket).
- [ ] **T6.5** Drag-drop: WebView's HTML5 drop fires for content; on
      Windows additionally hook `WM_DROPFILES` on the host HWND to recover
      file paths and forward to `window.aceDesktop.onFileDrop`.

## Phase 7 — CLI integration

- [ ] **T7.1** Add `desktop` subcommand to top-level CLI
      (`main.cpp` / `cli.cpp`). Flags: `--no-tray`, `--reuse-running-daemon`,
      `--debug` (open WebView devtools).
- [ ] **T7.2** Default subcommand on double-click: detect "no TTY + no
      args" via `isatty(stdout)` and re-exec as `acecode desktop` only if
      `acecode-desktop` is colocated. (Optional v1; otherwise rely on the
      packaged shortcut launching `acecode-desktop` directly.)

## Phase 8 — Docs

- [ ] **T8.1** New `docs/desktop.md` — user manual: install, launch,
      tray behavior, troubleshooting.
- [ ] **T8.2** New `docs/desktop-dev.md` — how to develop without
      rebuilding the desktop binary every iteration.
- [ ] **T8.3** Update `CLAUDE.md` "Architecture" section: add
      `src/desktop/` description, new daemon CLI flags, the bridge surface.
- [ ] **T8.4** Update `README.md` and `README_CN.md` with desktop
      install instructions.

## Phase 9 — Packaging & CI

- [ ] **T9.1** Windows: NSIS installer script bundling
      `acecode-desktop.exe` + `acecode.exe` + WebView2 bootstrapper.
- [ ] **T9.2** macOS: `.app` bundle layout, codesign + notarize steps in
      CI. Universal binary via `lipo`.
- [ ] **T9.3** Linux: `.deb` package + AppImage build. Document
      `libwebkit2gtk-4.1-0` runtime dep.
- [ ] **T9.4** Extend `.github/workflows/package.yml`: add
      `acecode-desktop` build matrix entries (Windows x64, macOS x64+arm64,
      Linux x64+arm64).
- [ ] **T9.5** Smoke job: launch `acecode-desktop --headless-smoke` (a new
      mode that boots, waits for daemon JSON line, navigates, and exits 0)
      to verify end-to-end on each OS.

## Phase 10 — Polish

- [ ] **T10.1** Dock/taskbar badge for unread tool-completion events.
- [ ] **T10.2** Global "Pause All Tools" tray action sends
      `POST /api/sessions/:id/abort` for every active session.
- [ ] **T10.3** Light/dark mode follows OS theme via `prefers-color-scheme`
      + a desktop-side override in `desktop_state.json`.
- [ ] **T10.4** First-run onboarding: if `~/.acecode/config.json` is
      missing, route to the existing `acecode configure` wizard but rendered
      in the WebView.

---

**Apply order**: Phase 1 → 2 in parallel (independent), then 3 → 4 → 5
sequential, then 6/7/8 in parallel, then 9, then 10 (post-launch).
