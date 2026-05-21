# ACECODE.md

This file provides guidance to acecode (https://github.com/tmoonlight/acecode) when working with code in this repository.

## Project Overview

ACECode is a C++17 AI coding agent with a terminal TUI, daemon HTTP/WebSocket API, bundled React web UI, Windows service mode, and optional desktop shell. The shared agent core is reused across surfaces; avoid fixing behavior in only one surface when the same rule belongs in shared code.

Use these durable references when the task touches their area:

- `README.md`: user-facing setup, run modes, prerequisites, and release-facing behavior.
- `ARCHITECTURE.md`: stable runtime map and source ownership.
- `AGENTS.md`: contributor rules, OpenSpec expectations, coding style, and test guidance.
- `CLAUDE.md`: legacy implementation memory and detailed subsystem notes. Keep it on disk unless explicitly asked to remove it.
- `docs/daemon-api.md`: daemon REST/WebSocket protocol. Update it when protocol behavior changes.
- `docs/model-context-resolution.md`: saved model, context-window, and resume rules.
- `docs/desktop-shell/multi-workspace.md`: desktop shell workspace/daemon behavior.

## Common Commands

Fetch submodules:

```bash
git submodule update --init --recursive
```

Build the embedded web UI before configuring CMake when frontend assets should be included in the daemon/desktop binaries:

```bash
cd web
pnpm install
pnpm test
pnpm build
cd ..
```

Configure a testable C++ build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports \
  -DBUILD_TESTING=ON
```

Common triplets are `x64-linux`, `arm64-linux`, `x64-windows-static`, `x64-osx`, and `arm64-osx`. Windows builds require libcurl 8.14 or newer for TLS behavior.

Build:

```bash
cmake --build build --config Release
```

Build and run all C++ tests:

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build --output-on-failure
```

Run a single discovered GoogleTest via CTest:

```bash
ctest --test-dir build -R <TestNameOrRegex> --output-on-failure
```

Run the unit-test binary directly with a GoogleTest filter when needed:

```bash
./build/tests/acecode_unit_tests --gtest_filter=<SuiteName.TestName>
.\build\tests\acecode_unit_tests.exe --gtest_filter=<SuiteName.TestName>
```

Run local quality checks:

```bash
scripts/code_quality_check.sh
scripts/code_quality_check.bat
```

Develop the web UI against a foreground daemon:

```bash
acecode daemon --foreground
cd web
pnpm dev
```

Build the optional desktop shell:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports \
  -DACECODE_BUILD_DESKTOP=ON

cmake --build build --target acecode-desktop
```

On Linux desktop builds, install the WebKitGTK development package before configuring. The desktop target expects the `acecode` daemon executable beside it at runtime.

## Architecture Boundaries

`main.cpp` owns the terminal TUI entry point: CLI parsing for interactive mode, provider/tool/command setup, FTXUI event loop wiring, and worker callbacks back into the UI thread. Keep reusable behavior out of `main.cpp` and place it in the relevant `src/<subsystem>/` area or an existing focused helper.

`src/agent_loop.cpp` is the multi-turn state machine. A text-only assistant reply ends the loop; `task_complete` is an optional explicit terminator. `AskUserQuestion` returns to the loop through an async prompter and is not a terminator. TUI and daemon turns should share this behavior.

`src/tool/` owns the tool registry and built-ins, including shell, file read/write/edit, grep/glob, task completion, skills, memory, MCP, structured user questions, and optional web search. Tool result summaries, metadata, and hunks are consumed by both TUI and web rendering, so keep structured result data centralized instead of formatting separately per surface.

`src/session/` persists canonical conversation messages as `<session-id>.jsonl` plus `<session-id>.meta.json`. Runtime-only display fields are not serialized; resume paths rebuild display rows, tool previews, summaries, and diffs from persisted messages and metadata. File-mutating tools should call the session checkpoint hook before writes so `/rewind` can restore user-turn state.

`src/daemon/` starts and supervises daemon mode, writes runtime pid/port/guid/token/heartbeat files, and owns foreground/detached/service lifecycle. `src/web/` owns Crow routes, WebSocket envelopes, auth, payload codecs, and static asset serving. Daemon session multiplexing goes through `SessionRegistry`; each session entry owns its own `SessionManager`, `PermissionManager`, `AgentLoop`, provider slot, async permission prompter, and question prompter.

`web/src/` is the React 18 + Vite + Tailwind v4 frontend. Prefer pure helpers in `web/src/lib/` for data shaping, protocol handling, markdown rendering, diff handling, and preference persistence. Do not edit generated frontend output directly; rebuild it with `pnpm build`.

`src/desktop/` is an optional native shell around daemon-backed webviews. It manages workspace registry, per-workspace daemon processes, bridge calls, tray menu, notifications, close-to-tray policy, and single-instance behavior. It should not require daemon internals to know about multiple workspaces.

`src/provider/` centralizes `LlmProvider` implementations, provider factory/swap logic, Copilot auth, OpenAI-compatible streaming, saved model profiles, and context-window resolution. TUI `/model` and daemon model switching should both use the shared `apply_model_to_session` path.

`src/network/proxy_resolver.*` centralizes proxy behavior for cpr call sites. Startup initializes the resolver and may probe proxy reachability; `/proxy` is a session-level override and should not persist config changes unless the command explicitly supports that.

`src/skills/`, `src/memory/`, and `src/project_instructions/` provide optional prompt context. Skills are discovered from configured global, project, and external directories and loaded lazily through `skill_view` or slash-command expansion. Memory writes are constrained to `~/.acecode/memory/`.

## Test And Build Structure

`acecode_testable` is the shared object library for headless logic used by production binaries and tests. The `acecode` executable links it plus TUI/markdown sources. The `acecode_unit_tests` target is discovered from `tests/**/*_test.cpp` and uses GoogleTest. Keep TUI-heavy and desktop-webview-only behavior isolated unless logic can be moved into a pure helper covered through `acecode_testable`.

Tests live under `tests/` and usually mirror source paths. Use focused tests for serializers, parsers, validators, handler helpers, provider/model helpers, permission logic, and headless state machines. Web-only changes should at minimum run `pnpm test` and `pnpm build` from `web/`.

## Protocol And Persistence Notes

Loopback daemon requests bypass token auth. Non-loopback requests require `X-ACECode-Token` or `?token=`, and non-loopback dangerous mode is rejected. WebSocket events use envelopes with `type`, `seq`, `timestamp_ms`, and `payload`; `EventDispatcher` assigns monotonic sequence numbers and keeps a bounded replay ring.

The daemon and TUI share session storage under the ACECode data directory. Current sessions use canonical shared JSONL files; old `<session-id>-<pid>.jsonl` files are unsupported experimental data and should not be migrated.

Saved model selection resolves through named `saved_models`, `default_model_name`, per-project overrides, and resumed session metadata. If protocol or metadata behavior changes, update the relevant docs and tests alongside the code.

## Repository Rules To Preserve

For non-trivial behavior changes, create or continue an OpenSpec change under `openspec/changes/` before implementation and update its `tasks.md` as tasks are completed.

Keep TUI-specific code in `main.cpp`, `src/tui/`, and `src/markdown/`. Keep daemon/API work in `src/daemon/`, `src/web/`, and shared session/provider/tool helpers as appropriate. Keep frontend work under `web/src/`. Avoid modifying vendored or submodule trees such as `external/`, `hermes-agent/`, or `claudecodehaha/` unless the task explicitly targets them.

Follow `.editorconfig`: UTF-8, LF endings, final newline, 4-space indentation for C++/CMake, and 2-space indentation for JSON/YAML. Use C++17. Headers should generally live in `src/**/*.hpp` with matching `.cpp` files. Test files use the singular `_test.cpp` suffix.

Prefer existing helpers such as `ToolArgsParser`, `ToolErrors`, path/session utilities, provider/model helpers, web handler pure functions, and frontend `web/src/lib/` helpers instead of duplicating parsing, validation, or protocol logic.

This is a terminal UI project. Avoid emoji and ambiguous-width glyphs in C++ source, rendered UI, logs, and console output; prefer ASCII or width-stable symbols already used in the codebase.
