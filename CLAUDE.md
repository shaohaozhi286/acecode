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

- **`src/agent_loop.{hpp,cpp}`**: Multi-turn conversation state machine. Runs on a worker thread; communicates back to the TUI via callbacks. Handles streaming, tool execution ordering, abort/cancellation.

- **`src/provider/`**: Abstract `LlmProvider` interface with two implementations:
  - `OpenAiCompatProvider` — generic OpenAI-compatible REST+SSE client
  - `CopilotProvider` — GitHub Copilot with device-flow OAuth and background token refresh
  - `ModelContextResolver` — maps model names to context window sizes by consulting the bundled models.dev registry; respects `cfg.openai.models_dev_provider_id` as an explicit hint so proxy base URLs still match the right provider entry.
  - `models_dev_paths` / `models_dev_registry` — locate `<seed>/api.json` (env `ACECODE_MODELS_DEV_DIR` → `<exe_dir>/../share/acecode/models_dev` → `/usr/share/acecode/models_dev`) and load it once at startup into a shared `nlohmann::json`. Network refresh (`https://models.dev/api.json`) is opt-in via `config.models_dev.allow_network` or `/models refresh --network` and never written to disk. The seed is shipped from `assets/models_dev/`; CMake `install` copies it to `<prefix>/share/acecode/models_dev/`.

- **`src/tool/`**: `ToolExecutor` registry + six built-in tools: `bash_tool`, `file_read_tool`, `file_write_tool`, `file_edit_tool`, `grep_tool`, `glob_tool`. Each tool carries a JSON schema for its parameters and a `ToolResult` return type. `ToolResult` now includes an optional `summary: ToolSummary` field (`verb` / `object` / `metrics` / `icon`) that the TUI uses to render a one-line summary row instead of the full output; tools that do not populate `summary` fall back to the legacy 10-line fold path. `bash_tool` / `file_read_tool` / `file_write_tool` / `file_edit_tool` all populate it. `ToolExecutor::build_tool_call_preview(name, args_json)` generates the compact `"tool  preview"` string the agent loop stores in `ChatMessage::display_override` so the TUI can render `→ bash  npm install` instead of `[Tool: bash] {JSON}`. Tool `execute` functions take `(args_json, const ToolContext&)` — the context carries an optional `stream(chunk)` callback and a pointer to `AgentLoop::abort_requested_`. `bash_tool` uses both: its polling loop pushes cleaned output chunks (UTF-8-boundary-safe, ANSI-stripped, `\r`-normalised via `src/utils/stream_processing.hpp`) through `ctx.stream`, and polls `ctx.abort_flag` every 10ms so Esc kills the subprocess within ~1s. `bash_tool` also accepts an optional `stdin_inputs: string[]` parameter: on POSIX it opens a stdin pipe and writes each entry with a trailing `\n` via a dedicated thread (for interactive commands like `apt install` confirmations); on Windows the parameter is accepted but silently ignored pending future work. When total captured output exceeds 100 KB, `bash_tool` now applies a head+tail truncation policy (first 40% + `[... N bytes omitted ...]` marker + last 60%) rather than single-tail truncation, preserving early context (build args, paths) that pure-tail truncation would lose. Icons are picked by `tool_icons.hpp::tool_icon()` which defaults to Unicode glyphs (✍ / ✎ / →) and falls back to ASCII letters (W / E / R) when `ACECODE_ASCII_ICONS` is set.

- **`src/permissions.hpp`**: `PermissionManager` with three modes (`Default`, `AcceptEdits`, `Yolo`). Read-only tools are auto-approved; write/exec tools prompt unless rules match. Rules are glob-pattern based.

- **`src/session/`**: `SessionManager` persists conversation history as JSONL (one JSON object per line) plus a metadata sidecar. Sessions are lazily created on the first message.

- **`src/commands/`**: `CommandRegistry` dispatches slash commands (`/help`, `/clear`, `/model`, `/models`, `/compact`, `/micro-compact`, `/configure`, `/session`). Each command receives a `CommandContext` with full app state. `/models` (info / refresh [--network] / lookup) inspects the bundled models.dev registry. `acecode configure` consumes the registry through `src/utils/models_dev_catalog.{hpp,cpp}`: the wizard's first menu now lists `Copilot (GitHub)` / `Browse models.dev catalog (N providers)` / `Custom OpenAI compatible`. Catalog browsing, the OpenAI model picker, and the Copilot model picker all route through `src/commands/configure_picker.{hpp,cpp}`, an FTXUI-backed helper that supports arrow-key + PgUp/PgDn + Home/End navigation, digit jump-select (with a 500 ms two-digit window), `/` substring filtering, and `c` / digit `0` for the custom-entry escape hatch. When stdout is not a TTY the helper falls back to the legacy numbered `n`/`p`/`q` text flow for piped / scripted invocations. `acecode_unit_tests` links `ftxui::component` (alongside the existing `ftxui::screen` / `ftxui::dom`) so the picker's symbols resolve at link time even though tests only exercise the pure `format_picker_row` formatter. The catalog browsing auto-fills `base_url` and probes `provider.env` for an existing API key. Selecting from the catalog persists `cfg.openai.models_dev_provider_id` so the resolver can keep reporting the right context window even after the user repoints `base_url`.

- **Tool-result rendering (`main.cpp`)**: When a `tool_result` row is rendered in the conversation view, a `TuiState::Message` with `summary.has_value() && !expanded` renders as a single green (success) or red (failure) summary line built from the tool's `ToolSummary`: `icon verb · object · m1 · m2 …`. Failed tools additionally render up to 3 leading lines of `ToolResult.output` dimmed underneath the summary so stderr is visible without expanding. Pressing `Ctrl+E` while a summarized tool_result is focused in the chat view toggles `expanded`, which switches the row to the legacy 10-line fold path for full-diff/output inspection (Ctrl+E falls through to the readline-style "move caret to end of input" binding when no chat row is focused). Sessions persisted before this change have no `summary` and so continue to render with the 10-line fold. `tool_call` rows prefer `ChatMessage::display_override` (e.g. `→ file_edit  src/foo.cpp`) over the raw `[Tool: X] {JSON}` form. These runtime-only fields (`summary`, `expanded`, `display_override`) are never written to session JSONL — `src/session/session_serializer.cpp` uses an explicit field allowlist.
- **`src/tui/tool_progress.{hpp,cpp}`**: Live tool-progress renderer. While `state.tool_running` is true, `render_tool_progress()` replaces the thinking animation with a 5-line-tail + status-line block (tool name, command preview, last 5 output lines, `+N more lines`, elapsed seconds, cumulative bytes). `render_tool_timer_chip()` is a compact `◑ bash  23s` chip slotted into the bottom status bar so the timer stays visible even when overlays cover the main progress element. Data is pushed by `AgentLoop` via `on_tool_progress_start/update/end` callbacks; `main.cpp` throttles re-renders to ≥150 ms between updates. The same file also exposes `render_thinking_timer_chip()`, a `○ Thinking  14s  ~82 tok` chip shown in the bottom bar while the agent is waiting on the LLM (`state.is_waiting && !tool_running`). The token segment appears only after `SHOW_TOKENS_AFTER_MS` (3000 ms) and prefers `state.last_completion_tokens_authoritative` (exact, from `on_usage`) over a `state.streaming_output_chars / 4` estimate (tilde-prefixed). The existing `anim_thread` 300 ms tick drives refresh while waiting; the chip disappears on `on_busy_changed(false)`.

- **`src/tui/slash_dropdown.{hpp,cpp}`**: Autocomplete dropdown shown above the input while the buffer starts with `/` and contains no whitespace. `refresh_slash_dropdown()` is called under `TuiState::mu` after every edit to `input_text` (character input, backspace, history recall, submit-clear); it reads the unified `CommandRegistry` (built-ins + skills), ranks by prefix > substring(name) > substring(description), caps at 8 items + "+N more" footer, and preserves the selected command name across filter updates. The event handler in `main.cpp` intercepts `↑/↓/Ctrl+P/Ctrl+N` (cyclic move), `Return/Tab` (commit = replace buffer with `/<name> `, no submit), and `Esc` (close + set `dismissed_for_input` until the buffer leaves command position). Suppressed while the resume picker or tool-confirmation dialog is active.

- **`src/tui_state.hpp`**: Central shared state for the TUI — message list, input buffer, thinking animation flags, tool-confirmation overlays, session picker, slash-command dropdown, the live `ToolProgress` state (`tool_name`, `command_preview`, `tail_lines`, `current_partial`, `total_lines`, `total_bytes`, `start_time`) used by `src/tui/tool_progress.*`, and the waiting-indicator state (`thinking_start_time`, `streaming_output_chars`, `last_completion_tokens_authoritative`) used by `render_thinking_timer_chip`. The waiting-indicator fields are reset on every `on_busy_changed(true)` transition and are only meaningful while `is_waiting` is true.

- **`src/prompt/system_prompt.hpp`**: Builds the dynamic system prompt from working directory info and the tool registry's auto-generated descriptions.

- **`src/markdown/`**: Lexer + formatter that converts markdown to ANSI escape sequences for terminal display, including syntax highlighting for code blocks.

- **`src/utils/`**: `logger.hpp` (file log to `acecode.log`), `token_tracker.hpp` (cumulative cost), `path_validator.hpp` (prevents escaping project root), `encoding.hpp` (UTF-8 helpers), `uuid.hpp` (session IDs), `stream_processing.hpp` (`strip_ansi`, `utf8_safe_boundary`, `feed_line_state` used by `bash_tool` to clean streaming output), `base64.hpp` (header-only standard base64 encoder, no decoder — used by the right-click OSC 52 clipboard path).

- **`src/auth/github_auth.hpp`**: GitHub device-flow OAuth — generates user code, polls, persists token.

- **`src/skills/`**: User-authored `SKILL.md` documents discovered from built-in `.acecode/skills` and compatible `.agent/skills` roots (plus any `skills.external_dirs` in config). `SkillRegistry` scans at startup, reads only YAML frontmatter for the index, and lazily loads the body when invoked. Each skill is registered as a `/<skill-name>` slash command; the `skills_list` and `skill_view` tools expose the same set to the LLM for progressive-disclosure discovery. Disabled names in `config.skills.disabled` are skipped. `/skills reload` rescans disk.

- **`src/memory/`**: Cross-session persistent user memory under `~/.acecode/memory/`. `MemoryRegistry` scans `<name>.md` entry files at startup (each with `name`/`description`/`type` YAML frontmatter, where `type ∈ {user, feedback, project, reference}`), caches them in memory with a mutex, and rewrites `MEMORY.md` (the index) on every `upsert` / `remove`. Entry writes are atomic (temp file + rename). Two LLM-facing tools ship: `memory_read` (no args → full index + entries list; `{type}` → filtered; `{name}` → full body) and `memory_write` (name-sanitized, path-locked to the memory dir so even Yolo mode can't escape). The `PermissionManager` auto-approves `memory_write` in every non-Yolo mode because the tool hard-locks its target path. User-facing commands `/memory list|view|edit|forget|reload` are registered alongside other builtins, and `/init` (in `src/commands/init_command.{hpp,cpp}`) submits an ACECODE.md-authoring prompt to the agent loop so the LLM surveys the codebase and writes the file via `file_write_tool` / `file_edit_tool`; when no provider is configured the command falls back to writing a static four-section skeleton via `build_acecode_md_skeleton(cwd)`.

- **`src/project_instructions/`**: Loads `ACECODE.md` / `AGENT.md` / `CLAUDE.md` from the user's directory hierarchy into the system prompt every turn. `load_project_instructions(cwd, cfg)` first checks `~/.acecode/` (global layer), then walks from HOME down to `cwd` (outer-first), picking at most one file per directory based on `cfg.filenames` priority (default `["ACECODE.md", "AGENT.md", "CLAUDE.md"]`). Toggle switches `cfg.read_agent_md` / `cfg.read_claude_md` remove their corresponding name from the effective list at runtime (`ACECODE.md` has no toggle — it's native). Per-file (`max_bytes`), aggregate (`max_total_bytes`), and walk-depth (`max_depth`) caps guard against runaway prompts; truncation is explicit and logged. `build_system_prompt` injects the merged body as a `# Project Instructions` section after tool descriptions, with a framing sentence telling the LLM these are user-authored conventions, not system-level overrides.

### Threading Model

Three background threads besides the main FTXUI loop:
1. **Agent worker** — runs `AgentLoop`, blocked on LLM HTTP streaming
2. **Auth thread** — Copilot device-flow polling (only when needed)
3. **Animation thread** — drives the "thinking" spinner at fixed intervals

Callbacks from these threads post events back into the FTXUI `ScreenInteractive` event queue.

### Mouse Input

FTXUI mouse tracking is enabled by default (`main.cpp:703`, no explicit `TrackMouse(false)`), so the program receives wheel and click events via standard ANSI mouse-tracking escape sequences. The wheel handler at `main.cpp:1446-1457` advances `chat_focus_index` by ±1 message per notch, reusing the same `scroll_chat()` helper that `ArrowUp` / `ArrowDown` and `PageUp` / `PageDown` use — there is no separate scroll engine for the mouse. Note: enabling mouse tracking can in some terminals intercept native click-and-drag selection (`Shift+drag` is the universal bypass on Windows Terminal, GNOME Terminal, iTerm2, Alacritty, Kitty); on Windows Terminal in practice native selection still works, so no in-app hint is rendered. If a future user reports lost selection on a different terminal, the fallback is to reintroduce `screen.TrackMouse(false)` or expose a config opt-out.

**Drag-select + right-click copy**: FTXUI's own selection API drives a built-in drag-select (left-button press+drag) that is styled via `selectionBackgroundColor(Color::Blue) | selectionForegroundColor(Color::White)` on the `message_view`. `screen.SelectionChange([]{})` at `main.cpp:708` is registered purely to activate live selection tracking; `screen.GetSelection()` is read on right-click. Right-click-pressed in the TUI writes `ESC ] 52 ; c ; <base64> ESC \` to stdout (OSC 52) using the header-only `src/utils/base64.hpp` encoder, then updates `state.status_line` to `Copied N bytes to clipboard` — `N` is byte count, not codepoint count. The confirmation is auto-cleared ~2 s later by the `anim_thread` loop via `state.status_line_clear_at` / `state.status_line_saved`. Works on Windows Terminal, iTerm2, Kitty, Alacritty, WezTerm, recent xterm. Inside tmux add `set -g set-clipboard on` to `.tmux.conf` or the OSC 52 sequence is consumed by tmux and never forwarded. Empty selection on right-click is a silent no-op.

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

## CI / Release

GitHub Actions workflow at `.github/workflows/package.yml` builds for Linux x64/arm64, Windows x64, macOS x64/arm64. Releases are created automatically on version tags (`v*`).
