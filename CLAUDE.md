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

**No unit test infrastructure exists** — validation is manual/integration through the TUI.

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
  - `ModelContextResolver` — maps model names to context window sizes

- **`src/tool/`**: `ToolExecutor` registry + six built-in tools: `bash_tool`, `file_read_tool`, `file_write_tool`, `file_edit_tool`, `grep_tool`, `glob_tool`. Each tool carries a JSON schema for its parameters and a `ToolResult` return type. Tool `execute` functions take `(args_json, const ToolContext&)` — the context carries an optional `stream(chunk)` callback and a pointer to `AgentLoop::abort_requested_`. `bash_tool` uses both: its polling loop pushes cleaned output chunks (UTF-8-boundary-safe, ANSI-stripped, `\r`-normalised via `src/utils/stream_processing.hpp`) through `ctx.stream`, and polls `ctx.abort_flag` every 10ms so Esc kills the subprocess within ~1s. `bash_tool` also accepts an optional `stdin_inputs: string[]` parameter: on POSIX it opens a stdin pipe and writes each entry with a trailing `\n` via a dedicated thread (for interactive commands like `apt install` confirmations); on Windows the parameter is accepted but silently ignored pending future work.

- **`src/permissions.hpp`**: `PermissionManager` with three modes (`Default`, `AcceptEdits`, `Yolo`). Read-only tools are auto-approved; write/exec tools prompt unless rules match. Rules are glob-pattern based.

- **`src/session/`**: `SessionManager` persists conversation history as JSONL (one JSON object per line) plus a metadata sidecar. Sessions are lazily created on the first message.

- **`src/commands/`**: `CommandRegistry` dispatches slash commands (`/help`, `/clear`, `/model`, `/compact`, `/micro-compact`, `/configure`, `/session`). Each command receives a `CommandContext` with full app state.

- **`src/tui/tool_progress.{hpp,cpp}`**: Live tool-progress renderer. While `state.tool_running` is true, `render_tool_progress()` replaces the thinking animation with a 5-line-tail + status-line block (tool name, command preview, last 5 output lines, `+N more lines`, elapsed seconds, cumulative bytes). `render_tool_timer_chip()` is a compact `◑ bash  23s` chip slotted into the bottom status bar so the timer stays visible even when overlays cover the main progress element. Data is pushed by `AgentLoop` via `on_tool_progress_start/update/end` callbacks; `main.cpp` throttles re-renders to ≥150 ms between updates. The same file also exposes `render_thinking_timer_chip()`, a `○ Thinking  14s  ~82 tok` chip shown in the bottom bar while the agent is waiting on the LLM (`state.is_waiting && !tool_running`). The token segment appears only after `SHOW_TOKENS_AFTER_MS` (3000 ms) and prefers `state.last_completion_tokens_authoritative` (exact, from `on_usage`) over a `state.streaming_output_chars / 4` estimate (tilde-prefixed). The existing `anim_thread` 300 ms tick drives refresh while waiting; the chip disappears on `on_busy_changed(false)`.

- **`src/tui/slash_dropdown.{hpp,cpp}`**: Autocomplete dropdown shown above the input while the buffer starts with `/` and contains no whitespace. `refresh_slash_dropdown()` is called under `TuiState::mu` after every edit to `input_text` (character input, backspace, history recall, submit-clear); it reads the unified `CommandRegistry` (built-ins + skills), ranks by prefix > substring(name) > substring(description), caps at 8 items + "+N more" footer, and preserves the selected command name across filter updates. The event handler in `main.cpp` intercepts `↑/↓/Ctrl+P/Ctrl+N` (cyclic move), `Return/Tab` (commit = replace buffer with `/<name> `, no submit), and `Esc` (close + set `dismissed_for_input` until the buffer leaves command position). Suppressed while the resume picker or tool-confirmation dialog is active.

- **`src/tui_state.hpp`**: Central shared state for the TUI — message list, input buffer, thinking animation flags, tool-confirmation overlays, session picker, slash-command dropdown, the live `ToolProgress` state (`tool_name`, `command_preview`, `tail_lines`, `current_partial`, `total_lines`, `total_bytes`, `start_time`) used by `src/tui/tool_progress.*`, and the waiting-indicator state (`thinking_start_time`, `streaming_output_chars`, `last_completion_tokens_authoritative`) used by `render_thinking_timer_chip`. The waiting-indicator fields are reset on every `on_busy_changed(true)` transition and are only meaningful while `is_waiting` is true.

- **`src/prompt/system_prompt.hpp`**: Builds the dynamic system prompt from working directory info and the tool registry's auto-generated descriptions.

- **`src/markdown/`**: Lexer + formatter that converts markdown to ANSI escape sequences for terminal display, including syntax highlighting for code blocks.

- **`src/utils/`**: `logger.hpp` (file log to `acecode.log`), `token_tracker.hpp` (cumulative cost), `path_validator.hpp` (prevents escaping project root), `encoding.hpp` (UTF-8 helpers), `uuid.hpp` (session IDs), `stream_processing.hpp` (`strip_ansi`, `utf8_safe_boundary`, `feed_line_state` used by `bash_tool` to clean streaming output), `base64.hpp` (header-only standard base64 encoder, no decoder — used by the right-click OSC 52 clipboard path).

- **`src/auth/github_auth.hpp`**: GitHub device-flow OAuth — generates user code, polls, persists token.

- **`src/skills/`**: User-authored `SKILL.md` documents discovered from `~/.acecode/skills/<category>/<name>/SKILL.md` (plus any `skills.external_dirs` in config). `SkillRegistry` scans at startup, reads only YAML frontmatter for the index, and lazily loads the body when invoked. Each skill is registered as a `/<skill-name>` slash command; the `skills_list` and `skill_view` tools expose the same set to the LLM for progressive-disclosure discovery. Disabled names in `config.skills.disabled` are skipped. `/skills reload` rescans disk.

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
  "openai": { "base_url": "http://localhost:1234/v1", "api_key": "", "model": "local-model" },
  "copilot": { "model": "gpt-4o" },
  "context_window": 128000,
  "max_sessions": 50,
  "skills": {
    "disabled": [],
    "external_dirs": []
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
