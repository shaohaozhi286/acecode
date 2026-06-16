## Context

ACECode currently wires MCP only in the TUI startup path. `main.cpp` creates a `McpManager`, calls `connect_all(config)`, then immediately calls `register_tools(tools)`. The slow work happens inside `McpManager::register_tools()`: each configured server is connected, initialized, queried for tools, and then registered into `ToolExecutor`. A broken stdio server can hold the startup path for the cpp-mcp request timeout, which makes the terminal UI feel frozen before the user can type.

The daemon path already has a shared `ToolExecutor` per daemon process, but does not instantiate `McpManager`. Desktop launches or reuses that daemon, so desktop cannot gain reliable MCP support until MCP lifecycle ownership is moved out of TUI-specific code.

## Goals / Non-Goals

**Goals:**

- Let TUI render and accept input without waiting for MCP server startup.
- Keep MCP tools available to the model as soon as each server finishes startup.
- Preserve `/mcp` control commands and make status output reflect in-progress startup.
- Prevent a slow or broken MCP server from blocking the whole UI or other MCP servers.
- Shape the runtime as a surface-neutral component reusable by daemon and therefore desktop.
- Keep existing `mcp_servers` config compatible.

**Non-Goals:**

- Add a new MCP transport.
- Implement Codex-style cached MCP tool snapshots in this change.
- Implement progressive tool discovery or deferred MCP tool search.
- Redesign Web/Desktop MCP settings UI.
- Add OAuth or connector-specific MCP flows.

## Decisions

### D1 - Split MCP lifecycle into runtime state plus surface adapters

`McpManager` remains the owner of MCP clients and discovered tools, but startup logic is moved behind surface-neutral methods. The manager records configured servers synchronously, then starts background connection work. TUI and daemon supply small callbacks for status updates; they do not perform MCP protocol work themselves.

Alternative considered: keep all async orchestration in `main.cpp`. That would solve TUI startup latency but would leave daemon/desktop with no reusable path and would keep protocol state coupled to terminal rendering.

### D2 - Do not hold the manager mutex while doing MCP I/O

The current `connect_entry_locked()` performs process launch, HTTP/SSE setup, `initialize()`, and `get_tools()` while the manager lock is held. The async implementation should copy the server config under lock, release the lock for slow I/O, then reacquire the lock only to publish final state and register/unregister tool definitions.

This prevents `/mcp`, shutdown, or another server's completion from blocking behind a slow handshake.

### D3 - Register tools incrementally through the existing ToolExecutor

When a server becomes connected, its discovered tools are registered into the shared `ToolExecutor` under the executor's existing lock. `AgentLoop` already snapshots tool definitions for each provider request, so later turns automatically see tools that become ready after startup.

The manager must unregister any stale tools for a server before publishing a replacement tool set during reconnect, and tool execution must continue copying `ToolImpl` out of the executor before invocation so unregistering does not invalidate in-flight calls.

### D4 - Add an explicit startup gate for the first model request

TUI startup itself does not wait for MCP. Before the first agent submission is handed to `AgentLoop`, the TUI checks the MCP manager's startup state. If configured MCP servers are still starting, the TUI waits for a short bounded period for startup to settle, while allowing cancellation or skip behavior. If the wait times out or the user skips, the prompt is submitted with the currently registered tools and a visible warning is added.

Alternative considered: never wait. That is fastest but makes the first prompt silently miss MCP tools even when they become ready a moment later.

### D5 - Surface-visible state uses a small stable enum

MCP server status should include `starting`, `connected`, `failed`, `disabled`, and `cancelled` or `timed_out`. The `/mcp` command and startup status messages should use this state instead of inferring availability from tool count.

The enum belongs in the reusable MCP runtime, not TUI code, so daemon APIs can later expose the same state.

### D6 - Shutdown cancels startup before tearing down clients

Shutdown first marks the manager as stopping, cancels pending startup work where possible, joins background startup threads, then releases connected clients. This preserves the existing rule that MCP child processes are torn down after the agent loop stops, while also avoiding detached startup work touching destroyed TUI or daemon state.

### D7 - Daemon/Desktop reuse is a first-class boundary

The reusable runtime should require only:

- `AppConfig::mcp_servers`
- a process-owned `ToolExecutor`
- a status callback
- lifecycle calls: configure/start, disable/enable/reconnect, wait/settle, shutdown

TUI will provide a callback that appends system messages and posts a redraw. Daemon can provide a callback that logs and later emits WebSocket/API events. Desktop should not own MCP directly; it gets MCP through the daemon runtime.

## Risks / Trade-offs

- [Risk] First prompt can still run before all MCP tools are ready. -> Mitigation: add bounded first-turn wait plus explicit warning when proceeding early.
- [Risk] Background worker could outlive TUI state. -> Mitigation: callbacks capture weak or lifetime-stable state, and shutdown joins startup workers before `McpManager` destruction.
- [Risk] Reconnect/disable races with a server finishing startup. -> Mitigation: use per-server generation IDs; stale completions do not publish tools or state.
- [Risk] cpp-mcp stdio requests still use a long internal timeout. -> Mitigation: wrap startup work in ACECode-level timeout/cancellation state where possible and avoid blocking UI even when the underlying client thread takes longer to return.
- [Risk] Incremental registration changes model-visible tools between turns. -> Mitigation: keep per-request snapshots as-is; no in-flight provider request mutates its tool list.

## Migration Plan

1. Introduce the reusable manager lifecycle API while preserving the existing synchronous behavior for tests if needed.
2. Move TUI startup to the async path and add user-visible startup/settle messages.
3. Update `/mcp` to display in-progress and failed states.
4. Add daemon ownership of the same runtime after TUI behavior is stable, initially logging status and registering tools into daemon sessions' shared `ToolExecutor`.
5. Later Web/Desktop UI work can expose daemon MCP status and reload endpoints using the same runtime state.
