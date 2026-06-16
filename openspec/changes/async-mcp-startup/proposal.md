## Why

ACECode currently connects configured MCP servers synchronously during TUI startup, so a slow or broken MCP server can delay the usable terminal UI by many seconds. The same runtime gap also blocks a clean daemon/desktop MCP path: MCP lifecycle logic is tied to the TUI entry point instead of being a reusable runtime service.

## What Changes

- Make MCP startup non-blocking for the TUI: the terminal UI starts immediately while MCP servers initialize in the background.
- Track MCP server startup as explicit runtime states so users can inspect `starting`, `connected`, `failed`, `disabled`, and cancelled or timed-out servers.
- Register MCP tools into the shared `ToolExecutor` as each server becomes ready, without racing in-flight tool calls or command handlers.
- Add a first-turn coordination point so users can type immediately, while the first model request either waits briefly for MCP startup to settle or proceeds with a visible warning when MCP is still unavailable.
- Extract the startup/lifecycle mechanism so daemon sessions and desktop-backed daemon processes can reuse the same MCP manager instead of duplicating TUI-specific logic.
- Preserve existing `/mcp` enable/disable/reconnect behavior while making slow operations asynchronous or explicitly reported.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `mcp-client`: Add non-blocking MCP startup, startup-state reporting, first-turn coordination, and a runtime lifecycle boundary reusable by TUI and daemon/desktop surfaces.

## Impact

- Affected code: `src/tool/mcp_manager.*`, `src/tool/tool_executor.*`, `main.cpp`, `src/commands/builtin_commands.cpp`, and daemon startup code in `src/daemon/worker.cpp`.
- Affected user surfaces: TUI startup messages/status, `/mcp` command output, daemon/Web/Desktop MCP availability once the shared runtime service is adopted.
- Affected tests: MCP manager state transitions, tool registration under asynchronous startup, `/mcp` output, TUI startup behavior, and daemon runtime wiring.
- No breaking config changes are required; existing `mcp_servers` entries continue to load.
