# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A standalone single-file spike using [webview/webview](https://github.com/webview/webview) to open a native window hosting an HTML page from C++. Lives inside the larger ACECode tree (`N:\Users\shao\acecode\webview_demo`) but is **not** wired into ACECode's build — it is exploratory code for evaluating webview as a potential front-end host. The parent `acecode/CLAUDE.md` describes a different, unrelated project; do not confuse the two.

The directory is untracked in the parent git repo (alongside sibling spikes `claudecodehaha/` and `hermes-agent/`).

## Layout

- `src/main.cpp` — `WinMain`/`main` that constructs `webview::webview`, calls `set_html(...)` with an inline HTML page, and `bind`s a `close_app` JS function that calls `w.terminate()`.
- `CMakeLists.txt` — uses `FetchContent` to pull `webview/webview` from GitHub at `master`. **No vcpkg.**
- `build/` — out-of-tree CMake build dir. Last configured with the default `Visual Studio 17 2022` generator on Windows.

## Build & run

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\webview_demo.exe
```

Linux/macOS use the same commands; the executable is `build/webview_demo` (Linux) or a `.app` bundle (macOS, due to `MACOSX_BUNDLE`).

No tests, no lint, no CI.

## Linking notes

- The exe links **`webview::core_static`** — webview is statically embedded, so the binary is self-contained and does not need `webview.dll` shipped alongside it.
- `webview::core` is the headers-only INTERFACE alias and is **not** sufficient on its own; pick `core_static` (self-contained) or `core_shared` (requires `webview.dll` next to the exe).
- WebView2 itself is loaded at runtime from the system's WebView2 Runtime (built into Win10/11 / shipped with Edge); no extra files required.
- A spurious `vcpkg manifest was disabled, but we found a manifest file in N:\Users\shao\acecode\` warning appears during MSBuild — it is leaked from the **parent ACECode tree's** `vcpkg.json`. Harmless here; ignore.

## Relation to ACECode

The parent ACECode project ships its own daemon-mode browser front-end built with native Web Components against a Crow HTTP/WS server (`acecode daemon start` → `localhost:28080`). webview-as-host would be an alternative delivery mechanism for that UI, but no integration exists yet — this directory is purely a "does the library work" check.
