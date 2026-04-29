# ACECode Daemon API

Reference for the HTTP and WebSocket protocol exposed by `acecode daemon` /
`acecode service`. Audience: front-end (`add-web-chat-ui` change), CLI
integrators, monitoring scripts.

> **Spec source of truth**: `openspec/changes/add-web-daemon/`
> (`design.md` Decisions 2 / 3 / 7 / 8, `specs/daemon-http/spec.md`).

---

## 1. Connecting

### Discovering address & token

After `acecode daemon start` (or `acecode service start`), runtime files are
written to `<data_dir>/run/`:

| File | Content |
|---|---|
| `daemon.pid`  | numeric pid (one line) |
| `daemon.port` | numeric port (one line) |
| `daemon.guid` | UUID v4 (one line) |
| `heartbeat`   | JSON `{pid, guid, timestamp_ms}` (rewritten every 2 s) |
| `token`       | URL-safe base64 string (~43 chars), file mode 0600 |

`<data_dir>` is `~/.acecode/` for standalone daemons (`acecode daemon ...`)
and `%PROGRAMDATA%\acecode\` (Windows) /
`/Library/Application Support/acecode/` (macOS) /
`/var/lib/acecode/` (Linux) for SCM-installed services. The daemon's own
process picks the right root via `RunMode` (see CLAUDE.md →
`src/utils/paths.{hpp,cpp}`).

### Bind address & port

Default `127.0.0.1:28080` (`config.web.bind` / `config.web.port`). Daemon is
**fail-fast on port collision** — it logs the error and exits with code 3, no
fallback / no retry. If the port is occupied, change `web.port` in
`config.json` or stop the conflicting process.

### Authentication

| Bind | Token required? |
|---|---|
| `127.0.0.1` / `::1` (loopback) | Optional (loopback is the trust boundary) |
| Anything else | **Required** — startup is rejected without one |

Pass the token in either of:
- HTTP header: `X-ACECode-Token: <token>`
- WebSocket connect URL: `?token=<token>`

Reject paths return:
- HTTP: `401` + body `{"error": "no token"}` or `{"error": "bad token"}`
- WebSocket: handshake refused at `.onaccept()` (Crow returns HTTP 400)

`-dangerous + non-loopback` is a hard preflight reject (rc=2 at startup) —
the daemon will not run that combination at all.

---

## 2. HTTP endpoints

All responses are `application/json` unless noted. Schemas use
abbreviated TypeScript-like notation.

### `GET /api/health`

Liveness + identity probe. Always available.

**Response 200**:
```json
{
  "guid": "ea86842a-fb1c-4242-b2b4-74be2aff1058",
  "pid": 18204,
  "port": 28080,
  "version": "0.1.2",
  "cwd": "C:\\Users\\you\\projects\\foo",
  "uptime_seconds": 423
}
```

### `GET /api/sessions`

List active in-memory sessions plus historical sessions on disk for the
daemon's working directory. Active sessions are flagged.

**Response 200**:
```json
{
  "sessions": [
    {
      "id": "550e8400-...",
      "title": "first 30 chars of first user msg",
      "created_at_unix_ms": 1700000000000,
      "active": true,
      "model": "gpt-4o",
      "provider": "copilot"
    }
  ]
}
```

### `POST /api/sessions`

Create a new session. Body fields are optional; omitted fields fall back to
the daemon's defaults from `config.json` and the resolved `ModelEntry`.

**Request body**:
```json
{
  "model": "gpt-4o",                 // optional override
  "initial_user_message": "...",     // optional; if present, kicks off the agent loop immediately
  "auto_start": true                 // default true; if false, session waits for user_input over WS
}
```

**Response 201**:
```json
{ "session_id": "550e8400-..." }
```

### `DELETE /api/sessions/:id`

Aborts any in-flight tool / LLM call, joins the worker thread, removes the
session from the registry.

**Response 204** on success; 404 if no such id.

### `GET /api/sessions/:id/messages?since=N`

Fetch session content. Two modes by `since`:

- `since=0` (default) → full snapshot: `{messages: ChatMessage[], events: SessionEvent[]}`.
  `messages` is the canonical OpenAI-format history loaded from disk; `events`
  is whatever's still in the in-memory ring buffer (last 1024 events).
- `since=N` (N > 0) → reconnect-replay: just `{events: SessionEvent[]}` where
  every event has `seq > N`. Used by clients that already have history and
  only need the gap.

If the requested seq predates the ring-buffer start, you'll get an empty list
and should re-fetch with `since=0`.

### `GET /api/skills`

List skills the daemon registered at startup.

**Response 200**:
```json
{
  "skills": [
    {
      "name": "init",
      "command_key": "/init",
      "description": "Initialize ACECODE.md ...",
      "category": "builtin",
      "enabled": true
    }
  ]
}
```

### `GET /api/mcp` / `PUT /api/mcp`

Read / write the `mcp_servers` segment of `~/.acecode/config.json`. GET
**redacts** `auth_token` fields (returns `"***"`) so the daemon never leaks
secrets through the wire. PUT validates schema, writes the file, but does
NOT auto-reload connected MCP clients — response includes
`{"reload_required": true}`.

### `POST /api/mcp/reload`

v1: returns **501 Not Implemented**. Restart the daemon to pick up MCP
config changes. Full hot-reload is a follow-up change.

---

## 3. WebSocket protocol — `WS /ws/sessions/:id`

### Envelope

Every server→client AND client→server frame is a single JSON object:

```json
{
  "type": "<kind>",
  "seq": 42,                  // monotonic per session, server-assigned
  "timestamp_ms": 1700000000000,
  "payload": { /* type-specific */ }
}
```

`seq` lets clients track ordering and request replay on reconnect.

### Connection lifecycle

```
client → server : WS handshake (with ?token=... if non-loopback)
server checks auth in .onaccept()
client → server : { "type": "hello", "payload": { "session_id": "...", "since": 0 } }
server          : (optional) replays events with seq > since from ring buffer
                  then registers the connection as a live listener
client ↔ server : full duplex traffic (events / inputs / decisions / pings)
client → server : close OR abrupt disconnect
server          : unsubscribes from EventDispatcher; AgentLoop keeps running
                  (registry holds the session); next reconnect can resume
```

### Server → client message types

| `type` | When | `payload` |
|---|---|---|
| `Token`            | LLM streamed a content delta | `{ "delta": "..." }` |
| `ReasoningDelta`   | LLM streamed a reasoning_content fragment (DeepSeek thinking, OpenRouter `reasoning`, Qwen) | `{ "delta": "..." }` |
| `Message`          | A complete `ChatMessage` was added to history | `{ "message": ChatMessage }` |
| `ToolStart`        | `bash_tool` / `file_*` etc. started executing | `{ "tool": "bash", "args": {...}, "preview": "..." }` |
| `ToolUpdate`       | Streaming output from a long tool (mostly `bash_tool`) | `{ "chunk": "..." }` |
| `ToolEnd`          | Tool finished | `{ "tool": "...", "ok": true, "summary": ToolSummary?, "hunks": ToolHunks?, "output_tail": "..." }` |
| `PermissionRequest`| Tool needs user confirmation | `{ "request_id": "...", "tool": "...", "args": {...}, "options": ["allow","deny","allow_session"] }` |
| `Usage`            | LLM reported token usage | `{ "prompt_tokens": N, "completion_tokens": N, "total_tokens": N }` |
| `BusyChanged`      | Transition between idle / waiting / running | `{ "busy": true, "reason": "waiting_llm"|"running_tool" }` |
| `Done`             | Agent loop reached a terminator (text reply / `task_complete` / max_iterations / abort) | `{ "reason": "text"|"task_complete"|"abort"|"max_iters", "summary": "..."? }` |
| `Error`            | Something failed (provider error / tool exception / permission timeout) | `{ "code": "...", "message": "..." }` |

Permission timeout is special: `AsyncPrompter` waits 5 minutes; on timeout it
emits an `Error` event AND treats the request as deny, then the agent loop
continues with the deny result.

### Client → server message types

| `type` | When | `payload` | Notes |
|---|---|---|---|
| `hello`      | First frame after connect | `{ "session_id": "...", "since": 0 }` | Required; server ignores all other types until hello binds the session |
| `user_input` | Send a user message | `{ "text": "..." }` | Triggers a new agent-loop turn |
| `decision`   | Respond to a `PermissionRequest` | `{ "request_id": "...", "decision": "allow"|"deny"|"allow_session" }` | Unblocks the worker; ignored if request_id unknown / already answered |
| `abort`      | Cancel current turn | `{}` | Equivalent to TUI Esc — interrupts streaming + tool execution |
| `ping`       | Keep-alive | `{}` | Server replies `{ "type": "pong", ... }` |

Unknown `type` values get an `Error` reply — `{"code": "unknown_type", "message": "...""}`.

### Reconnect strategy

1. Save the highest `seq` you've processed
2. On reconnect, send `hello` with `since: <last_seq>`
3. Server replays buffered events with `seq > since` (up to 1024)
4. If the gap is bigger than the ring buffer, you'll get nothing for the
   gap — fall back to `GET /api/sessions/:id/messages?since=0` for full
   snapshot, then resume WS from the new tail seq

---

## 4. Error codes (HTTP)

| Status | Meaning |
|---|---|
| 200 | OK |
| 201 | Created (POST /api/sessions) |
| 204 | No Content (DELETE /api/sessions/:id) |
| 400 | Malformed JSON body, missing required field, bad config write |
| 401 | Auth missing/invalid |
| 404 | Unknown session id / route |
| 501 | Not Implemented (currently `/api/mcp/reload` only) |

Error bodies are `{"error": "human-readable string"}`.

---

## 5. Process exit codes

Useful for monitoring / supervisor scripts:

| rc | Where | Meaning |
|---|---|---|
| 0 | any | Normal exit |
| 1 | various CLI | Generic failure ("no daemon running" etc.) |
| 2 | `worker.cpp` | `preflight_bind_check` rejected (non-loopback without token, or `-dangerous + non-loopback`) |
| 3 | `worker.cpp` / `server.cpp` | Crow `app.run()` threw — typically port already in use |
| 4 | `worker.cpp` | Failed to write a runtime file (pid/port/guid/token) |
| 5 | `cli.cpp foreground` | `validate_config` returned errors |
| 6 | `cli.cpp start` | Another daemon already running (GUID mutex) |
| 7 | `cli.cpp start` | `spawn_detached` failed |
| 8 | `cli.cpp start` | Detached worker didn't write `daemon.pid` within 5 s |
| 9 | `cli.cpp stop` | `terminate_pid` didn't succeed within 10 s |
| 10 | `cli.cpp` / `service_win.cpp` | Unknown subcommand |
| 11 | `cli.cpp` / `service_win.cpp` | No subcommand passed (help printed) |
| 21 | `service_win.cpp` | `--service-main` invoked outside SCM context |
| 22 | `service_win.cpp` | `StartServiceCtrlDispatcher` failed (other error) |
| 24 | `service_win.cpp` | Access denied (need elevated PowerShell for install/uninstall/start/stop) |
| 25-33 | `service_win.cpp` | Other SCM API failures (see source for exact mapping) |
| 64 | `main.cpp` | `--service-main` on non-Windows |
| 65 | `main.cpp` | `service` subcommand on non-Windows (use systemd/launchd) |

---

## 6. Examples

### Health check
```bash
curl -s http://127.0.0.1:28080/api/health | jq
```

### Create session and send first message
```bash
TOKEN=$(cat ~/.acecode/run/token)        # or %PROGRAMDATA%\acecode\run\token in service mode

SID=$(curl -s -X POST http://127.0.0.1:28080/api/sessions \
  -H "X-ACECode-Token: $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"initial_user_message":"hi"}' | jq -r .session_id)

# then connect WS to ws://127.0.0.1:28080/ws/sessions/$SID
# (use websocat / wscat / browser DevTools)
```

### Reconnect with replay
```js
ws.send(JSON.stringify({
  type: "hello",
  payload: { session_id: SID, since: lastSeenSeq }
}));
```
