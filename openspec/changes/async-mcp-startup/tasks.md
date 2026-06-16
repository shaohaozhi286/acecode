## 1. Runtime State And API

- [x] 1.1 Extend MCP server runtime state to include starting, connected, failed, disabled, cancelled, and timed-out states with optional failure text.
- [x] 1.2 Split MCP configuration loading from connection work so `connect_all` or its replacement records server entries without performing slow I/O.
- [x] 1.3 Refactor MCP connection code so process launch, `initialize()`, and `get_tools()` happen without holding the manager mutex.
- [x] 1.4 Add per-server generation IDs so stale startup or reconnect completions cannot overwrite newer state or re-register old tools.
- [x] 1.5 Add a surface-neutral status callback type for startup, success, failure, cancellation, and timeout notifications.

## 2. Asynchronous Startup

- [x] 2.1 Add a manager method to start all configured MCP servers asynchronously against a supplied `ToolExecutor`.
- [x] 2.2 Register discovered MCP tools incrementally when each server becomes connected.
- [x] 2.3 Keep `/mcp enable`, `/mcp disable`, and `/mcp reconnect` correct under asynchronous startup.
- [x] 2.4 Add bounded startup-settle helpers for callers that need to wait for all currently starting servers.
- [x] 2.5 Ensure `shutdown()` cancels pending startup work, prevents late tool registration, and cleans connected clients safely.

## 3. TUI Integration

- [x] 3.1 Move TUI startup from synchronous MCP initialization to the asynchronous manager path after TUI state can receive status updates.
- [x] 3.2 Show TUI startup progress when MCP servers are configured and update the display when servers connect or fail.
- [x] 3.3 Update `/mcp` and `/mcp list` output to show starting, failed, disabled, timed-out, and connected states.
- [x] 3.4 Add first-turn coordination so the first prompt waits briefly for MCP startup to settle before building the provider request.
- [x] 3.5 Add a visible warning when the first prompt proceeds before all configured MCP servers finish startup.
- [x] 3.6 Add a right-sidebar MCP panel in the TUI that shows loading animation, per-server state, and connected tool counts when the regular sidebar is visible.

## 4. Daemon And Desktop Reuse Boundary

- [x] 4.1 Introduce a daemon-compatible MCP runtime owner that depends only on `AppConfig`, `ToolExecutor`, lifecycle methods, and status callbacks.
- [x] 4.2 Wire daemon startup to instantiate the shared MCP runtime without blocking HTTP/WebSocket server startup.
- [x] 4.3 Register daemon MCP tools into the daemon's shared `ToolExecutor` so daemon-created Web/Desktop sessions can see them on later turns.
- [x] 4.4 Keep Desktop MCP ownership delegated to the supervised daemon; do not add MCP clients to the desktop shell process.
- [x] 4.5 Leave `/api/mcp/reload` behavior unchanged unless the reusable runtime exposes enough safe state for hot reload in this change.

## 5. Tests And Validation

- [x] 5.1 Add unit tests for MCP manager state transitions, stale generation handling, and shutdown during startup.
- [x] 5.2 Add tests proving tool definitions snapshot consistently while MCP tools are registered or unregistered.
- [x] 5.3 Add command tests for `/mcp` output with starting, connected, failed, disabled, and no-tool servers.
- [x] 5.4 Add TUI-level or isolated state tests for startup status messages and first-turn warning behavior.
- [x] 5.5 Add daemon startup tests or smoke coverage proving daemon startup is not blocked by MCP initialization and daemon sessions can see registered MCP tools.
- [x] 5.6 Run the relevant CMake unit target and OpenSpec validation for `async-mcp-startup`.
- [x] 5.7 Validate the TUI sidebar build path after adding MCP loading/tool rendering.
