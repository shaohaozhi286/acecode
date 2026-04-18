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

- **`src/tool/`**: `ToolExecutor` registry + six built-in tools: `bash_tool`, `file_read_tool`, `file_write_tool`, `file_edit_tool`, `grep_tool`, `glob_tool`. Each tool carries a JSON schema for its parameters and a `ToolResult` return type.

- **`src/permissions.hpp`**: `PermissionManager` with three modes (`Default`, `AcceptEdits`, `Yolo`). Read-only tools are auto-approved; write/exec tools prompt unless rules match. Rules are glob-pattern based.

- **`src/session/`**: `SessionManager` persists conversation history as JSONL (one JSON object per line) plus a metadata sidecar. Sessions are lazily created on the first message.

- **`src/commands/`**: `CommandRegistry` dispatches slash commands (`/help`, `/clear`, `/model`, `/compact`, `/micro-compact`, `/configure`, `/session`). Each command receives a `CommandContext` with full app state.

- **`src/tui/slash_dropdown.{hpp,cpp}`**: Autocomplete dropdown shown above the input while the buffer starts with `/` and contains no whitespace. `refresh_slash_dropdown()` is called under `TuiState::mu` after every edit to `input_text` (character input, backspace, history recall, submit-clear); it reads the unified `CommandRegistry` (built-ins + skills), ranks by prefix > substring(name) > substring(description), caps at 8 items + "+N more" footer, and preserves the selected command name across filter updates. The event handler in `main.cpp` intercepts `↑/↓/Ctrl+P/Ctrl+N` (cyclic move), `Return/Tab` (commit = replace buffer with `/<name> `, no submit), and `Esc` (close + set `dismissed_for_input` until the buffer leaves command position). Suppressed while the resume picker or tool-confirmation dialog is active.

- **`src/tui_state.hpp`**: Central shared state for the TUI — message list, input buffer, thinking animation flags, tool-confirmation overlays, session picker, slash-command dropdown.

- **`src/prompt/system_prompt.hpp`**: Builds the dynamic system prompt from working directory info and the tool registry's auto-generated descriptions.

- **`src/markdown/`**: Lexer + formatter that converts markdown to ANSI escape sequences for terminal display, including syntax highlighting for code blocks.

- **`src/utils/`**: `logger.hpp` (file log to `acecode.log`), `token_tracker.hpp` (cumulative cost), `path_validator.hpp` (prevents escaping project root), `encoding.hpp` (UTF-8 helpers), `uuid.hpp` (session IDs).

- **`src/auth/github_auth.hpp`**: GitHub device-flow OAuth — generates user code, polls, persists token.

- **`src/skills/`**: User-authored `SKILL.md` documents discovered from `~/.acecode/skills/<category>/<name>/SKILL.md` (plus any `skills.external_dirs` in config). `SkillRegistry` scans at startup, reads only YAML frontmatter for the index, and lazily loads the body when invoked. Each skill is registered as a `/<skill-name>` slash command; the `skills_list` and `skill_view` tools expose the same set to the LLM for progressive-disclosure discovery. Disabled names in `config.skills.disabled` are skipped. `/skills reload` rescans disk.

### Threading Model

Three background threads besides the main FTXUI loop:
1. **Agent worker** — runs `AgentLoop`, blocked on LLM HTTP streaming
2. **Auth thread** — Copilot device-flow polling (only when needed)
3. **Animation thread** — drives the "thinking" spinner at fixed intervals

Callbacks from these threads post events back into the FTXUI `ScreenInteractive` event queue.

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
