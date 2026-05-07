# CLAUDE.md

Implementation memory for coding agents working in this repository. For user-facing setup and run modes, use [README.md](README.md). For stable structure, use [ARCHITECTURE.md](ARCHITECTURE.md). For contributor rules, use [AGENTS.md](AGENTS.md).

## Current Runtime Surfaces

ACECode ships one main executable with terminal TUI and daemon subcommands, plus an optional desktop shell target.

- TUI mode starts from [main.cpp](main.cpp), configures provider/tools/commands, runs the FTXUI loop, and posts worker callbacks back into the UI event queue.
- Daemon mode starts from [src/daemon/cli.cpp](src/daemon/cli.cpp), converges on `worker.cpp::run_worker`, writes runtime files, then serves [src/web/](src/web) routes and WebSocket events.
- Web UI code lives in [web/src/](web/src), builds with React 18, Vite, Tailwind v4, `markdown-it`, `highlight.js`, and `diff2html`, then gets embedded from `web/dist/` by CMake.
- Desktop mode is opt-in through `-DACECODE_BUILD_DESKTOP=ON`; [src/desktop/](src/desktop) manages workspace registry, daemon pool, webview host, tray, notifications, and bridge calls.

## Build And Verification Notes

Use the command set in [AGENTS.md](AGENTS.md) as the source of truth. Important local facts:

- `acecode_testable` is the shared object library for headless logic and unit tests.
- `acecode` links `acecode_testable` plus FTXUI and TUI/markdown sources.
- `acecode_unit_tests` is available when `BUILD_TESTING=ON`.
- `acecode-desktop` is only created when desktop building is enabled.
- Rebuild [web/](web) with `pnpm build` before configuring CMake when embedded frontend assets need to change.
- Windows builds require libcurl 8.14 or newer for TLS behavior and use UTF-8 compile options.

## Agent Loop And Tools

[src/agent_loop.cpp](src/agent_loop.cpp) is the multi-turn state machine. A text-only assistant reply ends the loop. `task_complete` is an optional explicit terminator that renders a concise completion row. `AskUserQuestion` is not a terminator; its answer returns as a tool result. `config.agent_loop.max_iterations` is the hard cap.

Core tools are registered for both TUI and daemon paths: `bash`, `file_read`, `file_write`, `file_edit`, `grep`, `glob`, `task_complete`, `AskUserQuestion`, skill tools, memory tools, optional `web_search`, and MCP tools. `ToolResult` can carry summaries and hunks so TUI/web resume can render useful compact rows instead of raw output folds.

`bash_tool` streams cleaned output, polls abort state, truncates very large output, and supports POSIX `stdin_inputs`. File tools should preserve checkpoint hooks by calling `track_file_write_before` before mutating files.

## Sessions And Persistence

[src/session/](src/session) persists canonical conversation messages as JSONL with metadata sidecars. Runtime-only display fields are not serialized. Resume paths rebuild TUI pseudo-rows, tool previews, summaries, and diffs from persisted messages and metadata.

Rewind support uses per-user-turn checkpoints. `SessionManager::track_file_write_before` is the hook file-mutating tools call so `/rewind` can restore file state.

Daemon session multiplexing uses `SessionRegistry`. Each session entry owns its own `SessionManager`, `PermissionManager`, `AgentLoop`, async permission prompter, and question prompter. `EventDispatcher` gives each emitted event a monotonic sequence number and keeps a bounded replay ring.

## Skills, Memory, And Project Instructions

[src/skills/](src/skills) discovers `SKILL.md` files from configured global, project, and external skill directories. Skill metadata is read from YAML frontmatter at startup; full bodies are loaded lazily through skill invocation or the `skill_view` tool.

[src/memory/](src/memory) stores Markdown memory entries under `~/.acecode/memory/` and rewrites an index on upsert/remove. `memory_write` is constrained to that directory even under broad permission modes.

[src/project_instructions/](src/project_instructions) loads configured project-instruction filenames from the global config directory and then from the project hierarchy, outer-first, subject to per-file and aggregate byte caps. The repository root intentionally keeps only canonical docs; do not add duplicate root instruction files for this repository.

## Daemon And Web UI

Daemon startup writes pid, port, guid, token, and heartbeat files under `<data_dir>/run/`. Runtime file writes are atomic where practical, and daemon tokens are owner-only on supported platforms.

[src/web/server.cpp](src/web/server.cpp) registers health, sessions, messages, skills, MCP, model, history, files, commands, fork, and static asset routes. WebSocket payloads use envelopes with `type`, `seq`, `timestamp_ms`, and `payload`.

Loopback requests bypass daemon token auth. Non-loopback requests require `X-ACECode-Token` or `?token=`, and non-loopback dangerous mode is rejected. Keep [docs/daemon-api.md](docs/daemon-api.md) in sync for protocol changes.

The frontend has pure helpers under [web/src/lib/](web/src/lib) with Node-based tests. Prefer adding data-shaping logic there rather than embedding it directly in components.

## Desktop Shell

The desktop shell runs a webview against workspace-local daemon processes. It does not change daemon internals; each daemon still serves one current working directory. Workspace switching currently uses whole-page navigation so browser origin follows the active loopback port.

Key modules:

- `workspace_registry`: persisted workspace list and names.
- `daemon_pool`: per-workspace daemon process management.
- `web_host`: native webview wrapper and bridge binding.
- `tray_icon_win` and `notifications_win`: Windows tray and notification integration.

Detailed behavior is in [docs/desktop-shell/multi-workspace.md](docs/desktop-shell/multi-workspace.md).

## Network, Proxy, And Web Search

[src/network/proxy_resolver.*](src/network) centralizes proxy behavior for cpr call sites. `proxy_mode=auto` follows platform/system proxy settings; `off` forces direct; `manual` uses `proxy_url`. Startup can probe proxy reachability and temporarily fall back to direct if the configured proxy is unreachable.

`/proxy` shows or changes the session-level effective proxy state without persisting changes.

[src/tool/web_search/](src/tool/web_search) provides optional HTML-backed search with backend auto-detection and fallback. `/websearch` shows status, refreshes region detection, or switches backend for the current session. If `config.web_search.enabled=false`, the tool is not registered.

## Model Profiles And Context Windows

Model resolution layers are:

1. `default_model_name` from `saved_models`.
2. Per-project model override.
3. Resumed session provider/model metadata.
4. Legacy provider config fallback.

Context windows resolve through model profile data, bundled models.dev metadata, provider defaults, and configured fallbacks. The detailed rules live in [docs/model-context-resolution.md](docs/model-context-resolution.md).

## Config Notes

The config schema is intentionally sparse on write: defaults are omitted when possible. Notable sections are `saved_models`, `models_dev`, `skills`, `memory`, `project_instructions`, `agent_loop`, `daemon`, `web`, `network`, `web_search`, `tui`, `desktop`, and `mcp_servers`.

`mcp_servers` without `transport` default to stdio. `sse` is the legacy two-endpoint protocol. `http` is Streamable HTTP, defaulting to `/mcp` when no endpoint is provided.

## Useful Source Anchors

- [src/commands/builtin_commands.cpp](src/commands/builtin_commands.cpp): slash command registration and command help.
- [src/tool/tool_executor.cpp](src/tool/tool_executor.cpp): tool registry behavior, tool result message formatting, and compact call previews.
- [src/web/tool_event_payload.cpp](src/web/tool_event_payload.cpp): web serialization of tool progress and summaries.
- [src/web/message_payload.cpp](src/web/message_payload.cpp): REST/WS message payload identity and metadata.
- [src/session/session_serializer.cpp](src/session/session_serializer.cpp): persisted message field allowlist.
- [src/tui/render_mode.hpp](src/tui/render_mode.hpp): pure terminal render-mode decision logic.
- [src/utils/paths.cpp](src/utils/paths.cpp): user vs service data directory resolution.

## Maintenance Notes

- Keep root documentation role-based and short. Move detailed subsystem behavior into focused files under [docs/](docs).
- Avoid historical progress reports in root docs. Use OpenSpec changes and issue tracking for active work.
- When changing daemon routes, update protocol docs and add handler-level tests where possible.
- When changing frontend state-shaping behavior, add or update tests under [web/src/lib/](web/src/lib).
- When adding file-mutating tools, wire checkpoint tracking before the write so rewind remains reliable.
