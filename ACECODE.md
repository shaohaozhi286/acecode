# ACECODE.md

This file provides guidance to acecode (https://github.com/tmoonlight/acecode) when working with code in this repository.

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

## Testing

Configure with `-DBUILD_TESTING=ON` (default ON) and run:
```bash
cmake --build build --target acecode_unit_tests && ctest --test-dir build --output-on-failure
```

Tests live under `tests/`, mirror the `src/` layout, and file names end in `_test.cpp` (e.g. `src/utils/terminal_title.cpp` → `tests/utils/terminal_title_test.cpp`). `tests/CMakeLists.txt` globs `*_test.cpp` automatically. The test binary links against the `acecode_testable` OBJECT library, which holds every `src/*.cpp` **except** `src/tui/` and `src/markdown/`. `main.cpp` is always excluded.

## Running

```bash
./build/acecode                    # Fresh session
./build/acecode --resume [id]      # Resume a previous session
./build/acecode configure          # Run setup wizard
./build/acecode -dangerous         # Bypass all permission checks (use carefully)
```

## High-Level Architecture

### Data Flow
User input → TUI event handler → `AgentLoop` → `LlmProvider::chat_stream()` → SSE parse → streaming tokens back to TUI. 
When the LLM returns tool calls: `AgentLoop` invokes `PermissionManager` → `ToolExecutor` → result appended to message history → next LLM turn.

### Threading Model
Three background threads besides the main FTXUI loop:
1. **Agent worker** — runs `AgentLoop`, blocked on LLM HTTP streaming
2. **Auth thread** — Copilot device-flow polling
3. **Animation thread** — drives the "thinking" spinner at fixed intervals

### Key Components

- **`main.cpp`**: CLI arg parsing, terminal setup, provider/tool initialization, FTXUI rendering loop, event dispatch, shutdown.
- **`src/agent_loop.{hpp,cpp}`**: Multi-turn conversation state machine. Runs on a worker thread.
- **`src/provider/`**: `LlmProvider` interface with `OpenAiCompatProvider` and `CopilotProvider`. Includes `ModelContextResolver` mapping model names to context window sizes via bundled registry.
- **`src/tool/`**: `ToolExecutor` registry + built-in tools (`bash_tool`, `file_read_tool`, `file_write_tool`, `file_edit_tool`, `grep_tool`, `glob_tool`).
- **`src/permissions.hpp`**: `PermissionManager` (`Default`, `AcceptEdits`, `Yolo`). Write/exec tools prompt unless rules match.
- **`src/commands/`**: `CommandRegistry` dispatches slash commands (`/help`, `/clear`, `/model`, `/models`, `/compact`, `/configure`, `/session`).
- **`src/tui/` & `src/tui_state.hpp`**: Handles UI state, live tool progress rendering (`tool_progress.*`), and autocomplete dropdowns (`slash_dropdown.*`).
- **`src/skills/`**: Discovers user-authored `SKILL.md` documents. Lazily loads bodies into the LLM context.
- **`src/memory/`**: Cross-session persistent user memory under `~/.acecode/memory/`.
- **`src/project_instructions/`**: Loads `ACECODE.md` / `AGENT.md` / `CLAUDE.md` from the project hierarchy into the system prompt.
