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

### `GET /api/usage`

Aggregate durable token usage records for the Settings usage page. Records are
written only for usage observed after the usage ledger feature is installed;
historical session metadata is not backfilled into daily usage.

Query parameters:
- `days`: optional number of days to include, default `30`, clamped by the daemon.
- `workspace`: optional workspace hash. Use `__local__` for the compatibility
  working directory route.
- `timezone_offset_minutes`: optional JavaScript-style offset (`Date#getTimezoneOffset()`)
  used for daily buckets. Defaults to `0`.

**Response 200**:
```json
{
  "summary": {
    "records": 12,
    "estimated_records": 2,
    "session_count": 4,
    "totals": {
      "prompt_tokens": 120000,
      "completion_tokens": 18000,
      "total_tokens": 138000,
      "cache_read_tokens": 64000,
      "cache_write_tokens": 0,
      "reasoning_tokens": 1200
    }
  },
  "daily": [
    {
      "date": "2026-06-06",
      "tokens": 42000,
      "records": 3,
      "estimated_records": 0,
      "session_count": 1,
      "totals": { "total_tokens": 42000 }
    }
  ],
  "models": [
    {
      "label": "gpt-4o",
      "provider": "openai",
      "model": "gpt-4o",
      "model_preset": "gpt-4o",
      "records": 8,
      "estimated_records": 1,
      "session_count": 3,
      "totals": { "total_tokens": 110000 }
    }
  ],
  "workspaces": [
    {
      "workspace_hash": "abc123",
      "workspace_name": "repo",
      "cwd": "C:\\Users\\you\\repo",
      "records": 12,
      "estimated_records": 2,
      "session_count": 4,
      "totals": { "total_tokens": 138000 }
    }
  ],
  "metadata": {
    "days": 30,
    "period_start": "2026-05-08T00:00:00Z",
    "period_end": "2026-06-06T23:59:59Z",
    "timezone_offset_minutes": -480,
    "forward_only": true
  }
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

### `POST /api/sessions/:id/messages`

Queue a user input turn for an active session. The desktop/web composer sends
plain text plus optional uploaded attachment ids and structured contexts.

**Request body**:
```json
{
  "text": "Explain this code",
  "attachments": [{ "id": "att-..." }],
  "contexts": [
    {
      "type": "selection",
      "label": "README.md:23-24",
      "note": "2 lines",
      "text": "selected text...",
      "source": {
        "path": "C:/repo/README.md",
        "start_line": 23,
        "end_line": 24,
        "line_count": 2
      }
    }
  ]
}
```

`selection` contexts are model-visible request context, not visible prompt text.
The daemon expands them into the model input and records `metadata.display_text`
so the transcript continues to show only the user's typed text plus context
chips. `source.path` should be the absolute path used for model/tool
localization; `label` remains a compact display string. Unpinned/transient
selections are a client-side composer state and MUST NOT be sent in `contexts`.

**Response 202**:
```json
{ "queued": true }
```

### `DELETE /api/sessions/:id/todos`

Clear the current TodoWrite checklist for a session. The workspace-scoped alias
is `DELETE /api/workspaces/:hash/sessions/:id/todos`.

**Response 200**:
```json
{
  "session_id": "550e8400-...",
  "id": "550e8400-...",
  "workspace_hash": "abc123",
  "todos": [],
  "todo_summary": {
    "total": 0,
    "pending": 0,
    "in_progress": 0,
    "completed": 0,
    "cancelled": 0
  }
}
```

### `POST /api/sessions/:id/commands`

Execute daemon-owned builtin slash commands. This endpoint is intentionally
separate from `POST /api/sessions/:id/messages`: commands are not skill-expanded
and are not submitted as literal user messages. Supported command names are
limited to `init` and `compact`.

**Request body**:
```json
{
  "command": "init",
  "args": "",
  "display_text": "/init"
}
```

`command` may also be sent as slash text such as `"/compact"`. `display_text`
is preserved for the visible user-facing command label when the command enqueues
an LLM turn.

**Response 202**:
```json
{ "queued": true, "command": "compact" }
```

Errors:
- `400 {"error":"unsupported command","command":"..."}` for names other than
  `init` or `compact`.
- `404 {"error":"unknown session"}` when the session is not active in the
  daemon registry.

`/init` with a provider enqueues the same init prompt used by the TUI while
displaying `/init` in the transcript. Without a provider it writes the offline
`ACECODE.md` skeleton and emits a visible system message. `/compact` runs on the
AgentLoop worker queue and emits progress/completion/error messages. On success
it appends a hidden compact checkpoint to the session JSONL plus visible system
marker messages; older user-visible transcript rows remain available in history.
The checkpoint carries the provider-facing replacement history used for later
model requests and resume/fork reconstruction. Normal manual, auto, and rescue
compact success does not emit `transcript_replace`.

### `GET /api/sessions/:id/permissions`

Return the active session's permission mode. This is session-scoped; changing
one session does not affect other active sessions.

**Response 200**:
```json
{ "mode": "default", "description": "Prompt for write/exec tools" }
```

### `PUT /api/sessions/:id/permissions`

Switch the active session's permission mode. Valid `mode` values are
`default`, `accept-edits`, `plan`, and `yolo`. Session `yolo` auto-allows
normal tool permissions and allows local file tools to target paths outside the
workspace, but the first external file mutation in that session still requires
permission confirmation. Switching to `yolo` also resolves any already-open
permission prompt for that session with `allow`.

**Request body**:
```json
{ "mode": "yolo" }
```

**Response 200**:
```json
{ "mode": "yolo", "description": "Auto-allow tools; confirm first external file write" }
```

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

### `POST /api/open-in-explorer`

Open a directory in the OS file manager (Explorer / Finder / xdg-open).
Used by the web UI context menu in desktop webapp compatibility mode
(Edge app mode), where no webview bridge is available.

**Request body**: `{"path": "<absolute directory path, UTF-8>"}`

**Response 200**: `{"ok": true}`

**Errors**:
- `400` — missing/empty `path`, bad JSON, or validation failure
  (`{"ok": false, "error": "..."}`; e.g. path is not an existing directory,
  or lies outside registered workspaces and the daemon cwd).
- `501` — daemon was not started by the desktop shell
  (`native_folder_picker_enabled` off), endpoint unavailable.

### `GET /api/models`

List saved model profiles exposed to Web/Desktop. Responses never include
`api_key`. OpenAI-compatible entries may include unresolved
`request_headers` templates for editing.

**Response 200**:
```json
[
  {
    "name": "gateway",
    "provider": "openai",
    "model": "gpt-4o",
    "base_url": "https://gateway.example.com/v1",
    "request_headers": {
      "X-Team": "acecode",
      "X-Token": "{env:ACE_GATEWAY_TOKEN}"
    }
  }
]
```

### `POST /api/models` / `PUT /api/models/:name`

Create or update a saved model profile. OpenAI-compatible request bodies may
include `request_headers`, a JSON object of string header templates. Empty or
omitted `request_headers` is absent from the stored entry; for `PUT`, omitting
the field preserves the existing value, while sending `{}` clears it.

`Content-Type` is reserved by ACECode. `Authorization` is allowed and overrides
the bearer header derived from `api_key` when requests are sent.

### `POST /api/models/probe`

Probe remote model ids. For `provider: "openai"`, the request body accepts the
same unresolved `request_headers` templates and resolves `{env:NAME}` just
before sending the upstream `GET /models` request. Missing environment
variables return `400 {"error":"INVALID_REQUEST_HEADER"}` before any upstream
request is sent.

### `GET /api/config/ui-preferences`

Read non-sensitive Web/Desktop UI preferences. The legacy ACECode avatar
preference is retained for wire compatibility, but the current UI always hides
the ACECode avatar.

**Response 200**:
```json
{ "show_acecode_avatar": false }
```

### `PUT /api/config/ui-preferences`

Compatibility endpoint for older Web/Desktop clients. The request body is still
validated, but `show_acecode_avatar` is normalized to `false` and the avatar
remains hidden.

**Request body**:
```json
{ "show_acecode_avatar": false }
```

**Response 200** echoes the effective value:
```json
{ "show_acecode_avatar": false }
```

Errors:
- `400 {"error":"BAD_REQUEST"}` when `show_acecode_avatar` is missing or not a boolean.
- `500 {"error":"PERSIST_FAILED"}` when writing `config.json` fails.

### `GET /api/config/default-permission-mode`

Read the daemon default permission mode used by newly-created sessions.
Existing sessions remain session-scoped.

**Response 200**:
```json
{ "mode": "accept-edits", "description": "Auto-allow file edits, prompt for bash" }
```

### `PUT /api/config/default-permission-mode`

Persist the daemon default permission mode and update the in-memory session
template used for future new sessions. Valid `mode` values are `default`,
`accept-edits`, `plan`, and `yolo`.

**Request body**:
```json
{ "mode": "accept-edits" }
```

**Response 200** echoes the effective mode:
```json
{ "mode": "accept-edits", "description": "Auto-allow file edits, prompt for bash" }
```

Errors:
- `400 {"error":"BAD_REQUEST"}` when `mode` is missing or not a string.
- `400 {"error":"INVALID_PERMISSION_MODE"}` when `mode` is not recognized.
- `500 {"error":"PERSIST_FAILED"}` when writing `config.json` fails.

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
| `TranscriptReplace`| The server must replace the visible transcript for recovery/cleanup, such as retry or partial-stream cleanup; normal compact success does not use this event | `{ "messages": ChatMessage[] }` plus cleanup-specific fields |
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

## 7. Console PTY — `/api/pty` + `WS /ws/pty/:id`

Interactive terminal sessions hosted by the daemon (openspec change
`add-console-dock`). **All PTY routes are loopback-only**: requests from
non-loopback addresses are rejected with 403 regardless of token, because a
PTY executes commands without permission gating.

`GET /api/health` reports availability:

```json
"console": { "available": true, "backend": "conpty" }
```

`backend` is one of `conpty` (Windows 10 1809+), `winpty` (older Windows,
full TTY semantics via the bundled winpty agent), `pipe` (last-resort
fallback without TTY semantics — interactive programs do not work), or
`posix` (`forkpty`, Linux/macOS).

### HTTP endpoints

| Method | Path | Body | Response |
|---|---|---|---|
| POST | `/api/pty` | `{cwd?, title?}` | `201` session info; `429` at the 16-session limit |
| GET | `/api/pty` | — | `{backend, sessions: [...]}` |
| DELETE | `/api/pty/:id` | — | `204`; kills the shell process |
| POST | `/api/pty/:id/resize` | `{cols, rows}` (2..1000) | `204` |
| POST | `/api/pty/:id/title` | `{title}` | `204`; blank titles ignored, UTF-8-safe truncation at 200 bytes. The frontend forwards OSC 0/2 titles (xterm `onTitleChange`) here so reload-restored sessions keep their tab titles |

Session info shape:

```json
{
  "id": "pty-1", "title": "Terminal 1",
  "shell": "C:\Windows\system32\cmd.exe", "cwd": "...",
  "status": "running", "pid": 12345, "backend": "conpty",
  "exit_code": 0
}
```

`exit_code` is present only when `status == "exited"`. The default shell is
`%COMSPEC%` (cmd) on Windows and `$SHELL` on POSIX; override with the
`console.shell` config key. A PTY process survives independently of WebSocket
connections until it exits or is deleted; the daemon kills all sessions on
shutdown.

### WebSocket — `WS /ws/pty/:id?cursor=N`

Raw byte transport (no JSON envelopes, unlike `/ws/sessions`):

- **Server → client**: binary frames carry raw PTY output (VT byte stream,
  feed directly to a terminal emulator). Frames whose **first byte is
  `0x00`** are control frames: the remainder is UTF-8 JSON — `{"cursor": N}`
  (sent after the replay backlog; sync your local cursor) or
  `{"exit_code": N}` (process exited).
- **Client → server**: every frame (text or binary) is written verbatim to
  the PTY input (keyboard bytes).

Each session keeps a 2 MB rolling output buffer with a monotonically
increasing byte cursor. Connect with `cursor=N` to replay buffered output
from that offset (chunked at 64 KB) before the live stream; `cursor=-1`
skips the backlog. Reconnecting after a page reload with `cursor=0` replays
whatever the buffer still holds. Resize goes through the REST endpoint, not
the WebSocket.
