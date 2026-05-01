# add-desktop-shell — Proposal

> **Status**: Draft
> **Owner**: TBD
> **Related changes**: `add-web-daemon` (HTTP/WS surface), `add-web-chat-ui` (front-end components)

## What

Ship ACECode as a native desktop application (Windows / macOS / Linux) that
embeds a system-provided WebView and reuses the existing `acecode daemon` +
`web/` front-end. The desktop binary is a thin native shell — the agent loop,
sessions, tools, and UI all stay where they are today.

The new CLI entry point is:

```
acecode desktop                       # foreground GUI
acecode desktop --no-tray             # disable system tray
acecode desktop --reuse-running-daemon
```

Plus a "double-click the app icon" launch path on each OS.

## Why

The TUI is reported as hard to operate, especially for users unfamiliar with
keyboard-driven editors:

- Mouse interactions in FTXUI are limited (wheel scroll, shift-drag select);
  click-to-position cursor, native text-selection, and copy-paste are
  inconsistent across terminals (cmd.exe, Terminal.app, ConEmu, tmux).
- The tool-confirmation modal, AskUserQuestion modal, slash dropdown, and
  resume picker all share the same input surface, which creates a learning
  curve.
- Native affordances users expect are missing: drag-drop a file into chat,
  OS file picker for `file_read_tool` arguments, OS notification when a long
  bash run finishes, "always on top" / multi-monitor behavior, system tray
  for background sessions.

A web UI already exists (`web/` — 11 Custom Elements served by the daemon),
but launching it requires the user to (a) start the daemon manually and (b)
remember `http://localhost:28080`. A desktop wrapper makes the same UI a
first-class app, not a browser tab competing with bookmarks.

## Why not Tauri / Electron

| Option | Why it's wrong for this repo |
|---|---|
| **Electron** | Bundles Chromium (~120 MB per install) and forces an npm/Node toolchain. The current `web/` README explicitly chose hand-vendored Bootstrap to avoid npm — Electron would invalidate that decision and double the supply chain. |
| **Tauri** | Requires a Rust toolchain alongside CMake/vcpkg. Two-toolchain projects are a known maintenance tax (corrosion-rs partly fixes it but adds CMake complexity). The main payoff is a small bundle, but the bottleneck on bundle size is the daemon binary, not the shell. |
| **Wails** | Same toolchain-split problem with Go. |
| **CEF** | Heavy (Chromium copy) without Electron's ergonomics. |

We pick **`webview/webview`** (https://github.com/webview/webview): a small
C++ header-style library that wraps the OS-native WebView component
(WebView2 on Windows, WKWebView on macOS, WebKitGTK on Linux). It plugs into
the existing CMake/vcpkg flow with zero new toolchains, and the entire
desktop shell is a few hundred lines of C++.

## Non-goals (v1)

- **No front-end rewrite.** Components in `web/components/` stay as-is. We
  do not introduce React/Vue/Svelte. Markdown rendering follows the same v1
  decision (`textContent + pre-wrap`) until the web change adopts it.
- **No daemon-less mode.** The desktop binary always speaks to a daemon
  process; we do not embed the agent loop directly into the GUI process.
  Crash isolation matters more than saving one process.
- **No per-session native windows.** v1 is single-window with the existing
  sidebar UI for switching sessions. Multi-window is a follow-up.
- **No auto-update.** OS package managers / store / manual download for v1.
  Auto-update is a separate change.
- **No custom title bar / frameless window.** We use the OS-default chrome
  for v1 to avoid platform-specific HiDPI / drag-region bugs.

## Scope summary

What we _are_ building:

1. New target `acecode-desktop` (C++17, links `webview` + the existing
   `acecode_testable` OBJECT library for shared utilities).
2. New subcommand `acecode desktop` and a double-click bootstrapper.
3. Daemon supervision: spawn `acecode daemon --foreground --port 0
   --token-only-mode`, read the runtime files, hand the URL+token to the
   WebView.
4. Native menu, system tray, OS notifications, file/folder pickers, drag &
   drop, single-instance lock, deep-link handler (`acecode://`).
5. CMake packaging: Windows MSIX + standalone `.exe`, macOS `.app`
   (universal binary), Linux AppImage. Each shipped with the daemon binary
   colocated.

## Risks (covered in `design.md`)

- WebView2 Runtime not installed on older Windows 10 — bootstrapper.
- WebKitGTK 4.1 vs 6.0 ABI mismatch on Linux distros — pick 4.1 for now.
- Loopback bind being shared with other local processes — switch to
  per-launch token even on loopback.
- Port collision on the fixed `28080` — switch to `--port 0` for desktop
  launches and discover via runtime files.

## Acceptance

- `acecode desktop` opens a window showing the existing chat UI, signed in
  with the same daemon backend.
- Closing the window with system tray enabled keeps the daemon and any
  running agent turn alive; reopening reattaches.
- File dragged from Finder/Explorer onto the chat input inserts an absolute
  path string (or attaches it via a future protocol — see `design.md` §6).
- macOS: `cmd+,` opens settings; `cmd+q` quits cleanly (daemon SIGTERM).
- Windows: tray icon present; `--no-tray` disables it; closing the only
  window with tray off quits the process.
- Linux: AppImage launches on Ubuntu 22.04 / Fedora 39 with system-installed
  WebKitGTK 4.1.
- Three new test files under `tests/desktop/` (pure-logic ports of process
  supervision and runtime-file parsing) pass under `acecode_unit_tests`.
