# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ACECode is a terminal-based AI coding agent built in C++17. It provides an interactive TUI (Terminal User Interface) using FTXUI, supporting multi-turn conversations with automatic tool calling through OpenAI-compatible APIs or GitHub Copilot authentication.

## Build Instructions

**Prerequisites:** CMake >= 3.20, Ninja, vcpkg, a C++17-capable compiler.

```bash
# Initialize submodules
git submodule update --init --recursive

# Install dependencies via vcpkg (example triplet: x64-windows-static or x64-linux)
<vcpkg-root>/vcpkg install cpr nlohmann-json ftxui \
  --triplet <triplet> \
  --overlay-ports=$PWD/ports

# Configure
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports

# Build
cmake --build build --config Release
```

Output binary: `build/acecode` (Ninja) or `build/<config>/acecode` (multi-config generators).

**Note:** On Windows, libcurl >= 8.14 is required for TLS support. A custom FTXUI overlay port lives in `/ports/ftxui/`.

**Unit tests:** configure with `-DBUILD_TESTING=ON` (default ON) and run
```
cmake --build build --target acecode_unit_tests && ctest --test-dir build --output-on-failure
```
Tests live under `tests/`, mirror the `src/` layout, and file names end in `_test.cpp` (e.g. `src/utils/terminal_title.cpp` → `tests/utils/terminal_title_test.cpp`). `tests/CMakeLists.txt` globs `*_test.cpp` automatically — no CMake edit needed to add a new file. The test binary links against the `acecode_testable` OBJECT library, which holds every `src/*.cpp` **except** `src/tui/` and `src/markdown/` (they pull FTXUI and are exempted from unit testing). `main.cpp` is always excluded. Integration of TUI renders and `AgentLoop`/provider HTTP paths stays manual for now.

## Running

```bash
./build/acecode                    # Fresh session
./build/acecode --resume [id]      # Resume a previous session
./build/acecode configure          # Run setup wizard
./build/acecode -dangerous         # Bypass all permission checks (use carefully)
```

Config lives at `~/.acecode/config.json`. Sessions are stored under `.acecode/projects/<cwd_hash>/`.

## Architecture

### Data Flow

User input → TUI event handler → `AgentLoop` → `LlmProvider::chat_stream()` → SSE parse → streaming tokens back to TUI. When the LLM returns tool calls, `AgentLoop` invokes `PermissionManager` (may show a confirmation dialog) → `ToolExecutor` → result appended to message history → next LLM turn.

### Key Components

- **`main.cpp`** (1,558 lines): CLI arg parsing, terminal setup, provider/tool initialization, FTXUI rendering loop, event dispatch, shutdown. The TUI layout and all event wiring live here.

- **`src/agent_loop.{hpp,cpp}`**: Multi-turn conversation state machine. Runs on a worker thread; communicates back to the TUI via callbacks. Handles streaming, tool execution ordering, abort/cancellation. **Termination protocol** (`openspec/changes/align-loop-with-hermes`): a text-only assistant reply (zero tool calls) ends the loop — same default as hermes-agent (`run_agent.py:9823`) and claudecodehaha. When a non-Claude model hedges mid-task with "Would you like me to continue?", the loop ends and the user manually re-prompts (e.g. "继续"). `task_complete` is the OPTIONAL explicit terminator: model calls it with a `summary` string to render a compact "Done: <summary>" row and end the loop cleanly — but a plain text reply also ends the loop. `AskUserQuestion` is **NOT** a terminator: its answer flows back to the model as a tool_result and the loop continues. The only safety boundary is `config.agent_loop.max_iterations` (default 50, hard cap on total LLM turns per `run()`). Abort (Esc) is the user's instant short-circuit and emits a `[Interrupted]` system message.

- **`src/provider/`**: Abstract `LlmProvider` interface with two implementations:
  - `OpenAiCompatProvider` — generic OpenAI-compatible REST+SSE client. Supports reasoning-mode LLMs (DeepSeek thinking, Qwen, Moonshot, OpenRouter): `parse_sse_stream` accumulates `delta.reasoning_content` (DeepSeek primary) with `delta.reasoning` (OpenRouter/Qwen alias) as fallback into `ChatResponse::reasoning_content`, emits `StreamEventType::ReasoningDelta` per fragment (callback may ignore — agent loop currently drops it), and `parse_response` mirrors the same logic non-streaming. `build_request_body` echoes `reasoning_content` back on assistant messages whose stored value is non-empty, satisfying DeepSeek's hard contract (HTTP 400 `must be passed back to the API` otherwise). Field is omitted on user/system/tool roles and when empty, so non-reasoning servers see byte-identical request bodies. The field is persisted by `session_serializer` so `--resume` of a DeepSeek thinking session works on the next turn. See `openspec/changes/support-deepseek-reasoning`.
  - `CopilotProvider` — GitHub Copilot with device-flow OAuth and background token refresh
  - `ModelContextResolver` — maps model names to context window sizes by consulting the bundled models.dev registry; respects `cfg.openai.models_dev_provider_id` as an explicit hint so proxy base URLs still match the right provider entry.
  - `models_dev_paths` / `models_dev_registry` — locate `<seed>/api.json` (env `ACECODE_MODELS_DEV_DIR` → `<exe_dir>/../share/acecode/models_dev` → `/usr/share/acecode/models_dev`) and load it once at startup into a shared `nlohmann::json`. Network refresh (`https://models.dev/api.json`) is opt-in via `config.models_dev.allow_network` or `/models refresh --network` and never written to disk. The seed is shipped from `assets/models_dev/`; CMake `install` copies it to `<prefix>/share/acecode/models_dev/`.

- **`src/tool/`**: `ToolExecutor` registry + seven built-in tools: `bash_tool`, `file_read_tool`, `file_write_tool`, `file_edit_tool`, `grep_tool`, `glob_tool`, and `AskUserQuestion` (read-only; blocks the TUI with a multi-choice overlay — 1-4 questions × 2-4 options each, plus an auto-appended "Other..." for custom text; supports `multiSelect`. Registered in `src/tool/ask_user_question_tool.{hpp,cpp}` and uses the `ask_*` fields in `TuiState` with `ask_cv` mirroring the existing `confirm_pending` protocol. Overlay is rendered above `prompt_line` in `main.cpp` and takes priority over `confirm_pending` — the two are mutually exclusive by construction). Each tool carries a JSON schema for its parameters and a `ToolResult` return type. `ToolResult` now includes an optional `summary: ToolSummary` field (`verb` / `object` / `metrics` / `icon`) that the TUI uses to render a one-line summary row instead of the full output; tools that do not populate `summary` fall back to the legacy 10-line fold path. `bash_tool` / `file_read_tool` / `file_write_tool` / `file_edit_tool` all populate it. `ToolExecutor::build_tool_call_preview(name, args_json)` generates the compact `"tool  preview"` string the agent loop stores in `ChatMessage::display_override` so the TUI can render `→ bash  npm install` instead of `[Tool: bash] {JSON}`. Tool `execute` functions take `(args_json, const ToolContext&)` — the context carries an optional `stream(chunk)` callback and a pointer to `AgentLoop::abort_requested_`. `bash_tool` uses both: its polling loop pushes cleaned output chunks (UTF-8-boundary-safe, ANSI-stripped, `\r`-normalised via `src/utils/stream_processing.hpp`) through `ctx.stream`, and polls `ctx.abort_flag` every 10ms so Esc kills the subprocess within ~1s. `bash_tool` also accepts an optional `stdin_inputs: string[]` parameter: on POSIX it opens a stdin pipe and writes each entry with a trailing `\n` via a dedicated thread (for interactive commands like `apt install` confirmations); on Windows the parameter is accepted but silently ignored pending future work. When total captured output exceeds 100 KB, `bash_tool` now applies a head+tail truncation policy (first 40% + `[... N bytes omitted ...]` marker + last 60%) rather than single-tail truncation, preserving early context (build args, paths) that pure-tail truncation would lose. Icons are picked by `tool_icons.hpp::tool_icon()` which defaults to Unicode glyphs (✍ / ✎ / →) and falls back to ASCII letters (W / E / R) when `ACECODE_ASCII_ICONS` is set.

- **`src/permissions.hpp`**: `PermissionManager` with three modes (`Default`, `AcceptEdits`, `Yolo`). Read-only tools are auto-approved; write/exec tools prompt unless rules match. Rules are glob-pattern based.

- **`src/session/`**: `SessionManager` persists conversation history as JSONL (one JSON object per line) plus a metadata sidecar. Sessions are lazily created on the first message. **Resume visual restoration**: `session_replay.{hpp,cpp}` exposes the pure function `replay_session_messages(messages, tools) -> std::vector<TuiState::Message>` used by `main.cpp` to expand canonical OpenAI roles (`assistant`+`tool_calls`, `tool`) into the TUI's pseudo-role rows (`tool_call`, `tool_result`). `tool_metadata_codec.{hpp,cpp}` provides four pure functions `encode/decode_tool_summary` and `encode/decode_tool_hunks` that ride on the existing `ChatMessage.metadata` JSON channel under reserved subkeys `tool_summary` / `tool_hunks` — `agent_loop.cpp` writes them when `ToolResult.summary` / `ToolResult.hunks` are populated, and `replay_session_messages` reads them back so `--resume` fully restores green/red summary lines and colour diff hunks. Codec decoders are "lenient in, strict out": malformed JSON returns `std::nullopt` (not throws), so legacy / human-edited JSONL safely degrades to the gray fold path instead of crashing the resume.

- **`src/commands/`**: `CommandRegistry` dispatches slash commands (`/help`, `/clear`, `/model`, `/models`, `/compact`, `/micro-compact`, `/configure`, `/session`). Each command receives a `CommandContext` with full app state. `/models` (info / refresh [--network] / lookup) inspects the bundled models.dev registry. `acecode configure` consumes the registry through `src/utils/models_dev_catalog.{hpp,cpp}`: the wizard's first menu now lists `Copilot (GitHub)` / `Browse models.dev catalog (N providers)` / `Custom OpenAI compatible`. Catalog browsing, the OpenAI model picker, and the Copilot model picker all route through `src/commands/configure_picker.{hpp,cpp}`, an FTXUI-backed helper that supports arrow-key + PgUp/PgDn + Home/End navigation, digit jump-select (with a 500 ms two-digit window), `/` substring filtering, and `c` / digit `0` for the custom-entry escape hatch. When stdout is not a TTY the helper falls back to the legacy numbered `n`/`p`/`q` text flow for piped / scripted invocations. `acecode_unit_tests` links `ftxui::component` (alongside the existing `ftxui::screen` / `ftxui::dom`) so the picker's symbols resolve at link time even though tests only exercise the pure `format_picker_row` formatter. The catalog browsing auto-fills `base_url` and probes `provider.env` for an existing API key. Selecting from the catalog persists `cfg.openai.models_dev_provider_id` so the resolver can keep reporting the right context window even after the user repoints `base_url`.

- **Tool-result rendering (`main.cpp`)**: When a `tool_result` row is rendered in the conversation view, a `TuiState::Message` with `summary.has_value() && !expanded` renders as a single green (success) or red (failure) summary line built from the tool's `ToolSummary`: `icon verb · object · m1 · m2 …`. Failed tools additionally render up to 3 leading lines of `ToolResult.output` dimmed underneath the summary so stderr is visible without expanding. Pressing `Ctrl+E` while a summarized tool_result is focused in the chat view toggles `expanded`, which switches the row to the legacy 10-line fold path for full-diff/output inspection (Ctrl+E falls through to the readline-style "move caret to end of input" binding when no chat row is focused). Sessions persisted **before the tool-metadata codec** (`restore-tool-calls-on-resume`) have no `summary` / `hunks` payload and so continue to render with the 10-line fold; sessions written by this version onward fully restore both `summary` and `hunks` on resume via the `metadata.tool_summary` / `metadata.tool_hunks` JSON subkeys decoded by `src/session/tool_metadata_codec.cpp`. `tool_call` rows prefer `ChatMessage::display_override` (e.g. `→ file_edit  src/foo.cpp`) over the raw `[Tool: X] {JSON}` form; on resume `display_override` is **recomputed** from `(name, arguments)` via `ToolExecutor::build_tool_call_preview` rather than persisted. The runtime-only fields `expanded` and `display_override` are still never written to JSONL — `src/session/session_serializer.cpp` uses an explicit field allowlist that includes only `metadata` (the channel for `tool_summary` / `tool_hunks`) plus the canonical schema fields.
- **`src/tui/tool_progress.{hpp,cpp}`**: Live tool-progress renderer. While `state.tool_running` is true, `render_tool_progress()` replaces the thinking animation with a 5-line-tail + status-line block (tool name, command preview, last 5 output lines, `+N more lines`, elapsed seconds, cumulative bytes). `render_tool_timer_chip()` is a compact `◑ bash  23s` chip slotted into the bottom status bar so the timer stays visible even when overlays cover the main progress element. Data is pushed by `AgentLoop` via `on_tool_progress_start/update/end` callbacks; `main.cpp` throttles re-renders to ≥150 ms between updates. The same file also exposes `render_thinking_timer_chip()`, a `○ Thinking  14s  ~82 tok` chip shown in the bottom bar while the agent is waiting on the LLM (`state.is_waiting && !tool_running`). The token segment appears only after `SHOW_TOKENS_AFTER_MS` (3000 ms) and prefers `state.last_completion_tokens_authoritative` (exact, from `on_usage`) over a `state.streaming_output_chars / 4` estimate (tilde-prefixed). The existing `anim_thread` 300 ms tick drives refresh while waiting; the chip disappears on `on_busy_changed(false)`.

- **`src/tui/thick_vscroll_bar.{hpp,cpp}`**: Drop-in replacement for FTXUI's stock `vscroll_indicator`. Thumb glyph is upstream-identical (`┃ ╹ ╻`) and painted only in the rightmost reserved column; the decorator reserves `width = 3` columns so the leftmost 2 are blank-but-hit-testable (a wider hit zone for mouse aim). It also clamps the thumb height to a minimum of 3 cells (`min_thumb_2x = 6`) so long histories don't shrink the click target to a single half-block — `start_y` is recomputed against `scroll_range_2x = track_2x - size` so the clamped thumb still tracks scroll position proportionally without overshooting the track at max-scroll. The decorator subclasses `ftxui::Node` directly (replicating `NodeDecorator`'s minimal boilerplate since that header is private to FTXUI's source tree, not the public install) and writes its full reserved column range into a caller-owned `Box& out_track_box` each frame. The `width` parameter remains configurable for future experiments. Header-only `acecode::tui::y_to_focus(mouse_y, track_y_min, track_height, line_counts) -> {focus_index, line_offset}` is the pure mapping used by both the decorator and the mouse handler in `main.cpp`; unit-tested at `tests/tui/thick_vscroll_bar_test.cpp`. See "Draggable thick scrollbar" under Mouse Input.

- **`src/tui/picker_scroll.hpp`**: Header-only viewport-scroll helper shared by every keyboard-navigated overlay in the TUI. Exposes the pure function `acecode::tui::scroll_to_keep_visible(selected, prev_offset, visible_rows, total) -> int` (no FTXUI dep) plus three visible-row constants — `kResumePickerVisibleRows = 10`, `kRewindPickerVisibleRows = 10`, `kSlashDropdownVisibleRows = 8`. Each consumer (resume picker, rewind picker, slash dropdown) calls `scroll_to_keep_visible` after every selection change so the highlighted row stays inside the viewport, and renders only `[offset, offset + visible)` items with `↑ N more above` / `↓ M more below` indicator rows (reserved placeholder rows on the resume / rewind overlays keep the overlay height constant; the slash dropdown omits placeholders since it sits inline above the prompt). All three overlays support **ArrowUp/Down + PgUp/PgDn + Home/End** for viewport navigation. Digit-jump keys `1-9` (resume + rewind) retain absolute-index semantics — they always select `items[0..8]` regardless of the current view offset. Per-overlay specifics: `cmd_resume` no longer caps to 20 entries (`SessionManager::list_sessions()` already truncates to `config.max_sessions`); `cmd_rewind` no longer caps to `kMaxRewindItems = 20` (every collected user turn is rendered, viewport handles overflow); the slash dropdown no longer truncates to `kMaxItems = 8` and dropped the `(+N more)` footer in favor of viewport indicators. Unit tests live at `tests/tui/picker_scroll_test.cpp`.

- **`src/tui/slash_dropdown.{hpp,cpp}`**: Autocomplete dropdown shown above the input while the buffer starts with `/` and contains no whitespace. `refresh_slash_dropdown()` is called under `TuiState::mu` after every edit to `input_text` (character input, backspace, history recall, submit-clear); it reads the unified `CommandRegistry` (built-ins + skills), ranks by prefix > substring(name) > substring(description), keeps the **full** ranked list in `slash_dropdown_items` (no longer caps to 8), and preserves the selected command name across filter updates by recomputing `slash_dropdown_view_offset` via `picker_scroll::scroll_to_keep_visible`. Renders only the 8-row viewport plus optional `↑ N more above` / `↓ M more below` indicators. The event handler in `main.cpp` intercepts `↑/↓/Ctrl+P/Ctrl+N` (cyclic move with viewport follow), `PgUp/PgDn` (jump a page), `Home/End` (jump to first/last), `Return/Tab` (commit = replace buffer with `/<name> `, no submit), and `Esc` (close + set `dismissed_for_input` until the buffer leaves command position). Suppressed while the resume picker, rewind picker, tool-confirmation dialog, or AskUserQuestion overlay is active.

- **`src/tui_state.hpp`**: Central shared state for the TUI — message list, input buffer, thinking animation flags, tool-confirmation overlays, session picker, slash-command dropdown, the live `ToolProgress` state (`tool_name`, `command_preview`, `tail_lines`, `current_partial`, `total_lines`, `total_bytes`, `start_time`) used by `src/tui/tool_progress.*`, and the waiting-indicator state (`thinking_start_time`, `streaming_output_chars`, `last_completion_tokens_authoritative`) used by `render_thinking_timer_chip`. The waiting-indicator fields are reset on every `on_busy_changed(true)` transition and are only meaningful while `is_waiting` is true.

- **`src/prompt/system_prompt.hpp`**: Builds the dynamic system prompt from working directory info and the tool registry's auto-generated descriptions.

- **`src/markdown/`**: Lexer + formatter that converts markdown to ANSI escape sequences for terminal display, including syntax highlighting for code blocks.

- **`src/utils/`**: `logger.hpp` (file log to `acecode.log` in TUI mode; daemon mode swaps to `init_with_rotation(<data_dir>/logs/, "daemon", mirror_stderr)` for date-based file rotation, `set_clock_for_test` test seam), `token_tracker.hpp` (cumulative cost), `path_validator.hpp` (prevents escaping project root), `encoding.hpp` (UTF-8 helpers), `uuid.hpp` (session IDs), `stream_processing.hpp` (`strip_ansi`, `utf8_safe_boundary`, `feed_line_state` used by `bash_tool` to clean streaming output), `base64.hpp` (header-only standard base64 encoder, no decoder — used by the right-click OSC 52 clipboard path), `paths.{hpp,cpp}` (process-level `RunMode` enum + `resolve_data_dir(mode)`: `User` returns `~/.acecode/` for TUI / standalone daemon, `Service` returns `%PROGRAMDATA%\acecode\` on Windows / `/Library/Application Support/acecode/` on macOS / `/var/lib/acecode/` on Linux; `set_run_mode` is once-only with `LOG_WARN` on second call, `override_run_mode_for_test` + `reset_run_mode_for_test` are test-only escape hatches; `config.cpp::get_acecode_dir` delegates here so every `~/.acecode/...` read in the codebase honours the current mode without per-call-site changes).

- **`src/auth/github_auth.hpp`**: GitHub device-flow OAuth — generates user code, polls, persists token.

- **`src/daemon/`**: Background daemon that wraps the agent loop in an HTTP/WebSocket server (spec `openspec/changes/add-web-daemon`). Three startup modes converge on the same `worker.{hpp,cpp}::run_worker(opts, cfg)`: (1) `acecode daemon --foreground` (current console, logs mirror to stderr); (2) `acecode daemon start` (POSIX double-fork+setsid / Windows `DETACHED_PROCESS | CREATE_NO_WINDOW` detach via `platform.hpp::spawn_detached`); (3) `acecode service install` + `start` (Windows SCM via `service_win.{hpp,cpp}`, runs as LocalSystem with data root at `%PROGRAMDATA%\acecode\` per Decision 8). `worker.cpp` does the shared startup: load config → `validate_can_start` (GUID mutex — standalone rejects if a live `daemon.pid` exists; supervised validates `opts.guid == daemon.guid`) → write pid/port/guid/token to `<data_dir>/run/` → spin `HeartbeatWriter` (writes JSON `{pid, guid, timestamp_ms}` every 2 s, `Supervisor` reads `timestamp_ms` not mtime to dodge FAT32/network-share precision) → build `SessionRegistry` + `LocalSessionClient` + `WebServer` → block on `server.run()` until SIGTERM/SIGINT/CTRL_BREAK_EVENT or `request_worker_termination()` (called by `service_win.cpp::service_ctrl_handler` on SCM stop). `runtime_files.{hpp,cpp}` owns `daemon.pid` / `daemon.port` / `daemon.guid` / `heartbeat` / `token` — atomic `.tmp + rename`, token gets `restrict_permissions=true` (POSIX `chmod 0600` / Windows owner-only DACL via `atomic_file.hpp`). `cli.cpp` is the `acecode daemon ...` subcommand router; `service_win.cpp` is its Windows-only mirror for `acecode service install/uninstall/start/stop/status` plus the `ServiceMain` entry — first thing `service_main` does is `set_run_mode(RunMode::Service)` so all subsequent path resolution swings to ProgramData. Service identity is hardcoded `LocalSystem` + service name `AceCodeService` + display `ACECode Background Agent`; **no user/password is stored** — multi-user / per-user impersonation is deferred (Decision 8 known limitations).

- **`src/web/`**: Crow 1.3.2 HTTP/WebSocket server that exposes the daemon over `127.0.0.1:28080` (default; `web.port` configurable, port-in-use is fail-fast — daemon does NOT retry or fall back). `server.{hpp,cpp}` registers routes — `GET /api/health` (`{guid, pid, port, version, cwd, uptime_seconds}`), `GET/POST/DELETE /api/sessions`, `GET /api/sessions/:id/messages?since=N` (since=0 returns full `{events, messages}`; since>0 returns events-only delta for reconnect), `GET /api/skills`, `GET/PUT /api/mcp` (PUT writes config.json's `mcp_servers` segment without auto-reload, returns `reload_required:true`), `POST /api/mcp/reload` returns 501 in v1, `WS /ws/sessions/:id`. WebSocket envelope is `{type, seq, timestamp_ms, payload}` JSON: clients send `hello` (binds session + optional `since` triggers `EventDispatcher` replay), `user_input`, `decision` (permission response), `abort`, `ping`; server pushes `Token`, `ToolStart`/`ToolUpdate`/`ToolEnd`, `PermissionRequest`, `Done`, `Error` via `session_event_to_json`. `auth.{hpp,cpp}`: `require_auth(req)` is the per-route header line — loopback gets a free pass, non-loopback requires `X-ACECode-Token` header or `?token=` query; `preflight_bind_check(bind, token, dangerous)` runs at the top of `run_worker` and rejects (a) non-loopback bind without a generated token and (b) any `-dangerous + non-loopback` combo. WebSocket auth happens in `.onaccept()` so unauthorised upgrades fail before any frame flows. Bind failure path: `try/catch(std::exception)` around `app.run()` logs the underlying asio error plus a "port <N> may be in use; change web.port in config.json or stop the conflicting process; daemon will not retry" hint, returns rc=3, `cleanup_runtime_files()` deletes pid/port/heartbeat/token (guid is intentionally retained for forensic trace).

- **`src/session/` (daemon-side multiplexing)**: Built on top of the existing single-session `SessionManager` to let one daemon process host many concurrent sessions for many WebSocket clients. `SessionRegistry` (`session_registry.{hpp,cpp}`) is `unordered_map<session_id, unique_ptr<SessionEntry>>` under a mutex; each `SessionEntry` owns its own `SessionManager` + `PermissionManager` (cloned from a template) + `AgentLoop` + `AsyncPrompter`, so per-session state never crosses contamination. `SessionClient` (`session_client.hpp`) is the abstract surface (`create_session`, `list_sessions`, `subscribe`, `send_input`, `respond_permission`, `abort`); `LocalSessionClient` (`local_session_client.{hpp,cpp}`) is the same-process implementation the HTTP handlers call into — the abstraction reserves contract space for an eventual `RemoteSessionClient` on the browser side without requiring it now. `EventDispatcher` (`event_dispatcher.{hpp,cpp}`) is the per-session pub/sub bus: monotonic `seq`, 1024-capacity ring buffer, listener map; `subscribe(since_seq)` does an atomic replay-then-register so a reconnecting WS client fills any seq gap from buffer without losing the racy frames published while it was away. `permission_prompter.{hpp,cpp}` is the indirection that lets `AgentLoop` ask "may I run this tool?" in two different shapes: `CallbackPrompter` wraps the legacy synchronous TUI callback (modal dialog blocks on the AgentLoop thread); `AsyncPrompter` (daemon) emits a `PermissionRequest` event, blocks on a condvar with a 5-min timeout (timeout → emit Error event + treat as deny + 50 ms `abort_flag` poll for cancellation), resumes when `respond_permission(request_id, decision)` posts the answer. `AgentLoop::set_permission_prompter` swaps between them; without an injected prompter, the legacy callback path is preserved (TUI is zero-changed by the daemon work).

- **`src/skills/`**: User-authored `SKILL.md` documents discovered from built-in `.acecode/skills` and compatible `.agent/skills` roots (plus any `skills.external_dirs` in config). `SkillRegistry` scans at startup, reads only YAML frontmatter for the index, and lazily loads the body when invoked. Each skill is registered as a `/<skill-name>` slash command; the `skills_list` and `skill_view` tools expose the same set to the LLM for progressive-disclosure discovery. Disabled names in `config.skills.disabled` are skipped. `/skills reload` rescans disk.

- **`src/memory/`**: Cross-session persistent user memory under `~/.acecode/memory/`. `MemoryRegistry` scans `<name>.md` entry files at startup (each with `name`/`description`/`type` YAML frontmatter, where `type ∈ {user, feedback, project, reference}`), caches them in memory with a mutex, and rewrites `MEMORY.md` (the index) on every `upsert` / `remove`. Entry writes are atomic (temp file + rename). Two LLM-facing tools ship: `memory_read` (no args → full index + entries list; `{type}` → filtered; `{name}` → full body) and `memory_write` (name-sanitized, path-locked to the memory dir so even Yolo mode can't escape). The `PermissionManager` auto-approves `memory_write` in every non-Yolo mode because the tool hard-locks its target path. User-facing commands `/memory list|view|edit|forget|reload` are registered alongside other builtins, and `/init` (in `src/commands/init_command.{hpp,cpp}`) submits an ACECODE.md-authoring prompt to the agent loop so the LLM surveys the codebase and writes the file via `file_write_tool` / `file_edit_tool`; when no provider is configured the command falls back to writing a static four-section skeleton via `build_acecode_md_skeleton(cwd)`.

- **`src/history/`**: Per-working-directory persistent input history for the TUI ↑/↓ queue. `InputHistoryStore` (`input_history_store.{hpp,cpp}`) reads & writes `<project_dir>/input_history.jsonl` — the same `~/.acecode/projects/<cwd_hash>/` directory used by sessions, but the file is independent of any session JSONL (so `/clear`, deleting a session, or resuming a different one never loses the command-level history). `main.cpp` loads the file once at startup right after `TuiState` construction; a local `record_history(entry)` lambda (near the submit handler, ≈line 1600) centralises space-only suppression, adjacent-duplicate suppression, and the `InputHistoryStore::append` call so Normal and Shell (`!`) modes share identical semantics. `append` is append-first; when the file exceeds `config.input_history.max_entries` it does a head truncation via `<file>.tmp` + `std::filesystem::rename` (with a Windows `remove` + `rename` fallback). `load` is resilient: missing file → empty; malformed JSON / missing `text` field → skip-with-warning; never throws. The `/history` command (`src/commands/history_command.{hpp,cpp}`) lists the in-memory queue oldest-first or wipes both memory and disk with `/history clear` (uses the existing `status_line_saved` / `status_line_clear_at` auto-restore machinery for the 2-second "Input history cleared" flash). Toggleable via `config.input_history.enabled` (default `true`); when disabled the TUI falls back to pure in-memory history with zero disk IO.

- **`src/project_instructions/`**: Loads `ACECODE.md` / `AGENT.md` / `CLAUDE.md` from the user's directory hierarchy into the system prompt every turn. `load_project_instructions(cwd, cfg)` first checks `~/.acecode/` (global layer), then walks from HOME down to `cwd` (outer-first), picking at most one file per directory based on `cfg.filenames` priority (default `["ACECODE.md", "AGENT.md", "CLAUDE.md"]`). Toggle switches `cfg.read_agent_md` / `cfg.read_claude_md` remove their corresponding name from the effective list at runtime (`ACECODE.md` has no toggle — it's native). Per-file (`max_bytes`), aggregate (`max_total_bytes`), and walk-depth (`max_depth`) caps guard against runaway prompts; truncation is explicit and logged. `build_system_prompt` injects the merged body as a `# Project Instructions` section after tool descriptions, with a framing sentence telling the LLM these are user-authored conventions, not system-level overrides.

### Threading Model

**TUI mode** — three background threads besides the main FTXUI loop:
1. **Agent worker** — runs `AgentLoop`, blocked on LLM HTTP streaming
2. **Auth thread** — Copilot device-flow polling (only when needed)
3. **Animation thread** — drives the "thinking" spinner at fixed intervals

Callbacks from these threads post events back into the FTXUI `ScreenInteractive` event queue.

**Daemon mode** — no FTXUI loop; the main thread blocks in `Crow::App::run()` (which itself spawns an asio worker pool, configurable via `.multithreaded()`). Other threads:
1. **HeartbeatWriter** — single thread, wakes every `heartbeat_interval_ms` (default 2000), writes `<data_dir>/run/heartbeat`
2. **Term watcher** — single thread, blocks on `g_term_cv` until SIGTERM / Console handler / `request_worker_termination()` flips `g_term_requested`, then calls `server.stop()` to unblock Crow
3. **AgentLoop workers** — one per active session; `SessionRegistry::send_input` spawns a detached thread per turn, the thread emits events via `EventDispatcher` while it runs and exits when the turn (or abort) completes
4. **Crow internal pool** — handles HTTP requests; WebSocket frames are processed on the same connection-bound thread Crow assigns
5. **AsyncPrompter waiters** — when a tool needs permission, the AgentLoop worker is blocked on a condvar; the unblock is posted from a Crow handler thread (the one processing the `decision` WS frame). Cross-thread state is the per-session `pending_requests` map under `SessionRegistry::mu_`

### Mouse Input

FTXUI mouse tracking is enabled by default (`main.cpp:703`, no explicit `TrackMouse(false)`), so the program receives wheel and click events via standard ANSI mouse-tracking escape sequences. The wheel handler at `main.cpp:1446-1457` advances `chat_focus_index` by ±1 message per notch, reusing the same `scroll_chat()` helper that `ArrowUp` / `ArrowDown` and `PageUp` / `PageDown` use — there is no separate scroll engine for the mouse. Note: enabling mouse tracking can in some terminals intercept native click-and-drag selection (`Shift+drag` is the universal bypass on Windows Terminal, GNOME Terminal, iTerm2, Alacritty, Kitty); on Windows Terminal in practice native selection still works, so no in-app hint is rendered. If a future user reports lost selection on a different terminal, the fallback is to reintroduce `screen.TrackMouse(false)` or expose a config opt-out.

**Drag-select + right-click copy**: FTXUI's own selection API drives a built-in drag-select (left-button press+drag) that is styled via `selectionBackgroundColor(Color::Blue) | selectionForegroundColor(Color::White)` on the `message_view`. `screen.SelectionChange([]{})` at `main.cpp:708` is registered purely to activate live selection tracking; `screen.GetSelection()` is read on right-click. Right-click-pressed in the TUI writes `ESC ] 52 ; c ; <base64> ESC \` to stdout (OSC 52) using the header-only `src/utils/base64.hpp` encoder, then updates `state.status_line` to `Copied N bytes to clipboard` — `N` is byte count, not codepoint count. The confirmation is auto-cleared ~2 s later by the `anim_thread` loop via `state.status_line_clear_at` / `state.status_line_saved`. Works on Windows Terminal, iTerm2, Kitty, Alacritty, WezTerm, recent xterm. Inside tmux add `set -g set-clipboard on` to `.tmux.conf` or the OSC 52 sequence is consumed by tmux and never forwarded. Empty selection on right-click is a silent no-op.

**Draggable scrollbar**: `acecode::tui::thick_vscroll_bar` (`src/tui/thick_vscroll_bar.{hpp,cpp}`) replaces FTXUI's stock `vscroll_indicator`. The **thumb glyph is identical to upstream** (`┃ ╹ ╻` half-block at the rightmost reserved column), but the decorator reserves 3 columns total: the rightmost paints the thumb, the leftmost 2 are whitespace forming an invisible horizontal hit-zone extension so the mouse handler has 3 cells of click target instead of 1. The decorator also enforces a minimum thumb height of 3 cells (`min_thumb_2x = 6`) — long sessions still scale the thumb proportionally, but it never shrinks below a clickable size; `start_y` is recomputed so the clamped thumb still moves across the track in proportion to scroll position rather than overshooting at max-scroll. The decorator publishes its full reserved column range as `Box scrollbar_box` each frame (parallel to `chat_box`) so the mouse handler can hit-test a press as "scrollbar drag" instead of "text selection." Earlier iterations tried 2 columns + `│` rail + `█` thumb; the rail and `borderRounded` both used `│` and read as duplicated lines, so the visible part of the bar was reverted to upstream and only width and minimum-thumb-size were kept as ergonomics adjustments. Left-button Pressed inside `scrollbar_box` enters a "scroll drag" state (`TuiState::DragScrollbarPhase::Dragging`) and starts a jump-to-here motion: `acecode::tui::y_to_focus(mouse_y, track_y_min, track_height, snapshot)` maps the cursor to a `(focus_index, line_offset)` pair against a snapshot of `message_line_counts` taken at press time so streaming output mid-drag does not yank the thumb away from the cursor. Mutually exclusive with selection-drag — a press on the scrollbar column short-circuits the existing `drag_left_pressed` branch, so dragging the scrollbar never starts a text selection (and vice versa). On Released the snapshot is cleared and `drag_scrollbar_phase` returns to `Idle`; `chat_follow_tail` is **not** auto-restored, the user keeps whatever position they dragged to. Tail-follow only re-engages naturally when the user's drag lands the cursor on the last message (the existing `(idx == last_msg)` path). The pure `y_to_focus` mapping (header-only, no FTXUI dep) is unit-tested at `tests/tui/thick_vscroll_bar_test.cpp`. Wheel scroll, PgUp/PgDn, Home/End, drag-select, right-click-copy are all unchanged. Set `ACECODE_ASCII_ICONS` to fall back to `|` thumb on emulators that don't render Unicode block glyphs.

### Config Schema

`~/.acecode/config.json`:
```json
{
  "provider": "copilot",
  "openai": {
    "base_url": "http://localhost:1234/v1",
    "api_key": "",
    "model": "local-model",
    "models_dev_provider_id": "openrouter"
  },
  "copilot": { "model": "gpt-4o" },
  "context_window": 128000,
  "max_sessions": 50,
  "skills": {
    "disabled": [],
    "external_dirs": []
  },
  "memory": {
    "enabled": true,
    "max_index_bytes": 32768
  },
  "project_instructions": {
    "enabled": true,
    "max_depth": 8,
    "max_bytes": 262144,
    "max_total_bytes": 1048576,
    "filenames": ["ACECODE.md", "AGENT.md", "CLAUDE.md"],
    "read_agent_md": true,
    "read_claude_md": true
  },
  "models_dev": {
    "allow_network": false,
    "user_override_path": null,
    "refresh_on_command_only": true
  },
  "input_history": {
    "enabled": true,
    "max_entries": 10
  },
  "agent_loop": {
    "max_iterations": 50
  },
  "daemon": {
    "auto_start_on_double_click": false,
    "service_name": "ACECodeDaemon",
    "heartbeat_interval_ms": 2000,
    "heartbeat_timeout_ms": 15000
  },
  "web": {
    "enabled": true,
    "bind": "127.0.0.1",
    "port": 28080,
    "static_dir": ""
  },
  "saved_models": [
    {
      "name": "local-lm",
      "provider": "openai",
      "base_url": "http://localhost:1234/v1",
      "api_key": "x",
      "model": "llama-3"
    },
    {
      "name": "copilot-fast",
      "provider": "copilot",
      "model": "gpt-4o"
    }
  ],
  "default_model_name": "copilot-fast",
  "mcp_servers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/expose"]
    },
    "remote-sse": {
      "transport": "sse",
      "url": "https://mcp.example.com",
      "sse_endpoint": "/sse",
      "auth_token": "sk-...",
      "headers": { "X-Team": "acecode" },
      "timeout_seconds": 30,
      "validate_certificates": true
    },
    "remote-http": {
      "transport": "http",
      "url": "https://mcp-streamable.example.com",
      "sse_endpoint": "/mcp"
    }
  }
}
```

`mcp_servers` entries without a `transport` field are treated as `stdio` for backward compatibility. `sse` uses the legacy `/sse` + `/message` two-endpoint protocol; `http` uses the 2025-03-26 Streamable HTTP single-endpoint protocol (default path `/mcp`). They map to distinct client implementations inside `cpp-mcp`.

`saved_models` is a named registry of model entries (one provider + model + auth per row). `default_model_name` is a pointer into that array. Both fields are optional and default to empty; when empty, ACECode falls back to the legacy `provider` / `openai.*` / `copilot.*` fields at startup, so existing config.json files continue to work unchanged. Each `ModelEntry` requires `name` (non-empty, MUST NOT start with `(` — that prefix is reserved for ACECode-synthesized names like `"(legacy)"` and `"(session:<id>)"`), `provider` (`"openai"` or `"copilot"`), and `model`. `openai` entries additionally require non-empty `base_url` and `api_key`; `models_dev_provider_id` is optional. `load_config` rejects the file (prints to stderr + exits) on duplicate names, reserved prefixes, missing required fields, or a `default_model_name` that doesn't match any entry.

### Model profile resolution

At startup (and on every `--resume` / `/resume`), ACECode computes the effective `ModelEntry` via a three-layer resolver in `src/provider/model_resolver.cpp`:

1. `cfg.default_model_name` (if non-empty and found in `saved_models`)
2. `<cwd_hash>/model_override.json` `{"model_name": "..."}` (if present; `src/provider/cwd_model_override.cpp`)
3. Resumed `SessionMeta.provider` + `SessionMeta.model` (`--resume` only) — matched to `saved_models` by `(provider, model)` tuple, not by name. No match ⇒ build an ad-hoc entry with `name = "(session:<id_prefix>)"`, borrowing `base_url`/`api_key` from `cfg.openai` (best-effort), and append a one-line `⚠ Resumed with ad-hoc model entry …` system message to the conversation.

Empty / unresolved at every layer falls back to `synth_legacy_entry(cfg)` whose `name == "(legacy)"` — the picker always lists this row so users never lose access to their legacy config.

The `LlmProvider` itself is held in `main.cpp` as a `std::shared_ptr<LlmProvider>` protected by `std::mutex provider_mu`. `AgentLoop` receives a `ProviderAccessor` lambda that locks the mutex and copies the current `shared_ptr` — each worker turn takes a snapshot at the start so an in-flight `chat_stream` cannot dangle when the main thread swaps providers. `src/provider/provider_swap.cpp::swap_provider_if_needed` does the actual replace: same-provider entries reuse the instance (`set_model` + `OpenAiCompatProvider::reconfigure(base_url, api_key)`); cross-provider swaps destroy the old instance and call `create_provider_from_entry(entry)`; the context window is recomputed via `resolve_model_context_window` at the end.

### `/model` command — four modes

- `/model` — text-mode picker listing `saved_models` + the `(legacy)` fallback, marks the current effective row with `*`. (FTXUI overlay integration is follow-up work; the slash command runs on the main thread while `screen.Loop()` is blocking, so a proper overlay needs a picker state machine similar to the resume picker.)
- `/model <name>` — in-memory switch (no disk write), accepts any `saved_models` name or the reserved `(legacy)`.
- `/model --cwd <name>` — switch **and** persist to `~/.acecode/projects/<cwd_hash>/model_override.json`.
- `/model --default <name>` — switch **and** persist to `config.json`'s `default_model_name`.

Unknown name ⇒ error message `Unknown model name: <x>. Run /model to pick from available.`, no state change. All three persisting flags call `swap_provider_if_needed` under `provider_mu` and recompute `context_window`.

## CI / Release

GitHub Actions workflow at `.github/workflows/package.yml` builds for Linux x64/arm64, Windows x64, macOS x64/arm64. Releases are created automatically on version tags (`v*`).
