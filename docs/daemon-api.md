# ACECode Daemon Web API

This document describes the HTTP and WebSocket protocol exposed by
`acecode daemon` / `acecode service` for the Web and Desktop frontends.

Source of truth for this document:

- Route registration: `src/web/routes/routes_*.cpp`
- Shared response helpers: `src/web/server_helpers.cpp`
- Frontend callers: `web/src/lib/api.js`, `web/src/lib/connection.js`,
  `web/src/lib/consoleDock.js`

`OPTIONS` routes are CORS preflight helpers and are not listed as first-class
endpoints below.

---

## 1. Connecting

### Runtime files

After the daemon starts, runtime files are written to `<data_dir>/run/`:

| File | Content |
|---|---|
| `daemon.pid` | numeric pid |
| `daemon.port` | numeric port |
| `daemon.guid` | UUID v4 |
| `heartbeat` | JSON `{pid, guid, timestamp_ms}` refreshed periodically |
| `token` | URL-safe daemon token |

`<data_dir>` is `~/.acecode/` for standalone daemons and the platform service
data directory for installed services.

### Bind and auth

Default bind is `127.0.0.1:28080` (`config.web.bind` / `config.web.port`).
The daemon is fail-fast on port collision.

| Bind | Token required? |
|---|---|
| Loopback (`127.0.0.1`, `localhost`, `::1`) | Optional for same-origin loopback requests |
| Non-loopback | Required; startup is rejected without a token |
| Cross-origin loopback | Explicit token required |

Token locations:

- HTTP: `X-ACECode-Token: <token>`
- WebSocket: `?token=<token>`; browsers cannot set custom WS headers reliably

Auth failures return HTTP `401` with `{"error":"no token"}` or
`{"error":"bad token"}`. WebSocket handshakes are rejected.

### CORS

Loopback origins receive:

- `Access-Control-Allow-Origin: <origin>`
- `Access-Control-Allow-Headers: Content-Type, X-ACECode-Token`
- `Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS`

---

## 2. Common Shapes

### Error body

Most JSON errors are one of:

```json
{"error":"BAD_REQUEST","message":"human readable text"}
```

or:

```json
{"error":"human readable text"}
```

### Workspace

```json
{
  "hash": "16-char-cwd-hash",
  "cwd": "C:/repo",
  "name": "repo",
  "available": true
}
```

`__local__` is a compatibility workspace hash for the daemon cwd.

### Session summary

Session list endpoints return arrays of objects shaped like:

```json
{
  "id": "session-id",
  "active": true,
  "status": "idle",
  "workspace_hash": "abc123",
  "cwd": "C:/repo",
  "no_workspace": false,
  "title": "Investigate daemon routes",
  "title_source": "user",
  "summary": "latest user summary",
  "created_at": "2026-07-04T01:23:45Z",
  "updated_at": "2026-07-04T01:25:00Z",
  "provider": "openai",
  "model": "gpt-4.1",
  "model_name": "work-gpt",
  "model_preset": "work-gpt",
  "context_window": 128000,
  "deleted": false,
  "message_count": 12,
  "turn_count": 4,
  "permission_mode": "default",
  "token_usage": null,
  "session_token_usage": null,
  "todos": [],
  "todo_summary": {"total":0,"pending":0,"in_progress":0,"completed":0,"cancelled":0},
  "archived": false,
  "attention_state": "read",
  "read_state": "read",
  "busy": false,
  "status_cursor": 0,
  "update_cursor": 0,
  "read_cursor": 0
}
```

Some fields are omitted when empty, especially `todos` and token usage.

### Token usage

```json
{
  "prompt_tokens": 0,
  "completion_tokens": 0,
  "total_tokens": 0,
  "cache_read_tokens": 0,
  "cache_write_tokens": 0,
  "reasoning_tokens": 0,
  "has_data": false
}
```

### Session event

Server event frames and replayed events use:

```json
{
  "type": "message",
  "seq": 42,
  "timestamp_ms": 1783152000000,
  "session_id": "session-id",
  "workspace_hash": "abc123",
  "payload": {}
}
```

`payload.session_id`, `payload.workspace_hash`, and `payload.cwd` are injected
when known.

---

## 3. HTTP Endpoint Index

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/health` | daemon liveness and capabilities |
| GET | `/api/model-pool-status` | model pool load snapshot |
| GET | `/api/usage` | token usage aggregation |
| GET | `/api/history` | input history by cwd |
| POST | `/api/history` | append input history |
| GET | `/api/workspaces` | list registered workspaces |
| POST | `/api/workspaces` | register cwd as workspace |
| POST | `/api/workspaces/pick-folder` | desktop native folder picker |
| POST | `/api/open-in-explorer` | open folder in OS file manager |
| GET | `/api/workspaces/:hash/sessions` | list sessions in workspace |
| POST | `/api/workspaces/:hash/sessions` | create workspace session |
| POST | `/api/workspaces/:hash/sessions/:id/resume` | resume workspace session |
| PUT | `/api/workspaces/:hash/sessions/:id/archive` | archive workspace session |
| DELETE | `/api/workspaces/:hash/sessions/:id/archive` | unarchive workspace session |
| PUT | `/api/workspaces/:hash/sessions/:id/title` | set session title |
| GET | `/api/workspaces/:hash/sessions/:id/draft` | read composer draft |
| PUT | `/api/workspaces/:hash/sessions/:id/draft` | write composer draft |
| DELETE | `/api/workspaces/:hash/sessions/:id/todos` | clear session todos |
| GET | `/api/workspaces/:hash/opencode-import` | preview opencode import |
| POST | `/api/workspaces/:hash/opencode-import` | start opencode import job |
| GET | `/api/workspaces/:hash/opencode-import/:job_id` | poll opencode import job |
| GET | `/api/workspaces/:hash/pinned-sessions` | list pinned session ids |
| PUT | `/api/workspaces/:hash/pinned-sessions` | set pinned session ids |
| GET | `/api/pinned-sessions/order` | read cross-workspace pin order |
| PUT | `/api/pinned-sessions/order` | set cross-workspace pin order |
| GET | `/api/sessions` | compatibility session list |
| POST | `/api/sessions` | compatibility session create |
| POST | `/api/sessions/:id/resume` | compatibility session resume |
| DELETE | `/api/sessions/:id` | destroy active session |
| PUT | `/api/sessions/:id/archive` | archive compatibility session |
| DELETE | `/api/sessions/:id/archive` | unarchive compatibility session |
| PUT | `/api/sessions/:id/title` | set compatibility session title |
| GET | `/api/sessions/:id/draft` | read compatibility draft |
| PUT | `/api/sessions/:id/draft` | write compatibility draft |
| DELETE | `/api/sessions/:id/todos` | clear compatibility todos |
| GET | `/api/sessions/:id/messages` | transcript snapshot or event replay |
| POST | `/api/sessions/:id/messages` | queue user input |
| POST | `/api/sessions/:id/attachments` | upload session attachment |
| GET | `/api/sessions/:id/attachments/:attachment_id/blob` | download attachment bytes |
| POST | `/api/sessions/:id/commands` | run daemon builtin slash command |
| GET | `/api/sessions/:id/permissions` | read session permission mode |
| PUT | `/api/sessions/:id/permissions` | set session permission mode |
| GET | `/api/sessions/:id/model` | read session model state |
| POST | `/api/sessions/:id/model` | switch session model |
| POST | `/api/sessions/:id/fork` | fork a transcript prefix |
| POST | `/api/sessions/:id/file-checkpoints/:message_id/restore` | restore files to checkpoint |
| GET | `/api/files` | list directory |
| GET | `/api/files/content` | read text file |
| GET | `/api/files/blob` | read previewable binary file |
| GET | `/api/commands` | list slash commands |
| GET | `/api/skills/root` | resolve effective skills directory |
| GET | `/api/skills` | list registered skills |
| PUT | `/api/skills/:name` | enable or disable skill |
| GET | `/api/skills/:name/body` | read `SKILL.md` body |
| GET | `/api/hooks` | list hooks snapshot |
| POST | `/api/hooks/refresh` | reload hook registry |
| POST | `/api/hooks/:id/trust` | trust hook |
| POST | `/api/hooks/:id/disable` | disable hook |
| POST | `/api/hooks/:id/enable` | enable hook |
| GET | `/api/models` | list saved model profiles |
| POST | `/api/models` | add saved model profile |
| PUT | `/api/models/:name` | update saved model profile |
| DELETE | `/api/models/:name` | remove saved model profile |
| POST | `/api/models/probe` | probe provider model ids |
| GET | `/api/config/default-model` | read default saved model name |
| POST | `/api/config/default-model` | set default saved model |
| GET | `/api/copilot/auth` | read Copilot auth status |
| DELETE | `/api/copilot/auth` | delete saved GitHub token |
| POST | `/api/copilot/auth/device` | start GitHub device flow |
| POST | `/api/copilot/auth/device/poll` | poll device flow |
| GET | `/api/config/ui-preferences` | read UI preferences |
| PUT | `/api/config/ui-preferences` | write UI preferences |
| GET | `/api/config/custom-instructions` | read custom instructions |
| PUT | `/api/config/custom-instructions` | write custom instructions |
| GET | `/api/config/connectors` | read connector settings |
| PUT | `/api/config/connectors` | write connector settings |
| GET | `/api/config/default-permission-mode` | read default permission mode |
| PUT | `/api/config/default-permission-mode` | write default permission mode |
| GET | `/api/config/upgrade` | read update service config |
| PUT | `/api/config/upgrade` | write update service config |
| GET | `/api/update/status` | check update availability |
| POST | `/api/update/start` | start explicit self-update |
| GET | `/api/config/ace-browser-bridge` | read browser bridge settings |
| PUT | `/api/config/ace-browser-bridge` | write browser bridge settings |
| GET | `/api/mcp` | read MCP config |
| PUT | `/api/mcp` | write MCP config |
| POST | `/api/mcp/reload` | currently returns 501 |
| GET | `/api/feedback/desktop/recent-sessions` | list sessions for feedback attachment |
| POST | `/api/feedback/desktop` | package and upload desktop feedback |
| GET | `/api/pty/shells` | list console shell choices |
| GET | `/api/pty` | list PTY sessions |
| POST | `/api/pty` | create PTY session |
| DELETE | `/api/pty/:id` | remove PTY session |
| POST | `/api/pty/:id/resize` | resize PTY |
| POST | `/api/pty/:id/title` | set PTY title |
| PUT | `/api/console/config` | write console shell config |

---

## 4. Health, Usage, and History

### `GET /api/health`

No auth requirement. Returns daemon identity and non-sensitive frontend
capabilities.

```json
{
  "guid": "ea86842a-fb1c-4242-b2b4-74be2aff1058",
  "pid": 18204,
  "port": 28080,
  "version": "0.5.10",
  "cwd": "C:/repo",
  "uptime_seconds": 423,
  "notifications": {
    "enabled": true,
    "on_question": true,
    "on_completion": true,
    "suppress_when_focused": true
  },
  "features": {
    "completed_turn_self_heal": {"enabled": true}
  },
  "console": {
    "available": true,
    "backend": "conpty"
  }
}
```

`console.backend` is `conpty`, `winpty`, `pipe`, or `posix`.

### `GET /api/model-pool-status`

No auth requirement. Used by the chat UI to show model pool load.

```json
{
  "models": [
    {
      "modelPoolName": "pool-a",
      "usageRate": 0.42,
      "maxWindowTokens": 128000,
      "effectiveContextWindow": 128000
    }
  ]
}
```

### `GET /api/usage`

Query parameters:

- `days`: optional, defaults to `30`
- `workspace`: optional workspace hash; `__local__` means daemon cwd
- `timezone_offset_minutes`: optional JS `Date#getTimezoneOffset()` value

Returns usage summary, daily buckets, model buckets, workspace buckets, and
metadata. Durable usage is forward-only; older session metadata is not
backfilled.

### `GET /api/history?cwd=<cwd>&max=N`

Returns an array of input history strings for `cwd`.

### `POST /api/history`

Body:

```json
{"text":"last prompt"}
```

Appends to the daemon cwd input history. Returns `204`. If input history is
disabled, the write is silently ignored.

---

## 5. Workspaces and Sessions

### `GET /api/workspaces`

Returns `Workspace[]`. The registry is scanned before listing. If no registry
is available, the compatibility workspace may be returned.

### `POST /api/workspaces`

Body:

```json
{"cwd":"C:/repo"}
```

Registers the cwd and returns `201` plus a `Workspace`. Errors:

- `400` bad JSON or missing `cwd`
- `503` workspace registry unavailable

### `POST /api/workspaces/pick-folder`

Desktop-only native folder picker. Returns a registered `Workspace` or `null`
when the user cancels. Errors:

- `501` native folder picker unavailable
- `503` registry or callback unavailable

### `POST /api/open-in-explorer`

Body:

```json
{"path":"C:/repo"}
```

Opens an absolute directory in Explorer/Finder/xdg-open. The desktop callback
validates that the path exists and is within an allowed root: a registered
workspace, the daemon cwd, or the user-global skills directory
(`~/.acecode/skills`, for the settings page "open global skills directory"
button). Returns `{"ok":true}`. Returns `501` when the daemon has no desktop
callback.

### `GET /api/workspaces/:hash/sessions?archived=1`

Returns `SessionSummary[]` for a workspace. Without `archived=1`, active and
unarchived disk sessions are returned. With `archived=1`, only archived disk
sessions are returned.

### `POST /api/workspaces/:hash/sessions`

Creates a session in the workspace. Body fields are optional:

```json
{
  "model": "saved-model-name",
  "name": "saved-model-name",
  "permission_mode": "default",
  "permissionMode": "default",
  "initial_user_message": "hello",
  "auto_start": true,
  "no_workspace": false,
  "noWorkspace": false
}
```

`model` and `name` are aliases. `permission_mode` and `permissionMode` are
aliases. `auto_start` defaults to `false`; it only starts a turn when an
`initial_user_message` is present. Returns:

```json
{
  "session_id": "sid",
  "id": "sid",
  "workspace_hash": "abc123",
  "cwd": "C:/repo"
}
```

Errors include `404` unknown workspace, `409` workspace path unavailable,
`400` invalid permission mode, and `503` session client unavailable.

### `POST /api/workspaces/:hash/sessions/:id/resume`

Loads an existing disk session into the current daemon registry. Returns
`{"session_id","id","active":true,"workspace_hash","cwd"}`. Errors:

- `404` workspace or session not found
- `409` session is active in another live process, old incompatible PID data,
  or workspace path unavailable
- `503` session client unavailable

### Compatibility session routes

The following routes operate on the daemon compatibility workspace:

- `GET /api/sessions?archived=1`
- `POST /api/sessions`
- `POST /api/sessions/:id/resume`
- `PUT /api/sessions/:id/archive`
- `DELETE /api/sessions/:id/archive`
- `PUT /api/sessions/:id/title`
- `GET /api/sessions/:id/draft`
- `PUT /api/sessions/:id/draft`
- `DELETE /api/sessions/:id/todos`

`GET /api/sessions` returns `SessionSummary[]`, not a wrapper object.

Sub-agent sessions (created by the `spawn_subagent` tool; their meta carries a
persisted `parent_session_id`) are excluded from session lists by default so
they never appear in the sidebar or the global search. Query them explicitly:

- `GET /api/sessions?parent=<session_id>` — only the sub-agent sessions spawned
  by that parent (background-tasks panel data source). The active part of the
  merge skips workspace filtering; the disk part still scans the requested
  workspace's project directory.
- `GET /api/workspaces/:hash/sessions?parent=<session_id>` — same semantics on
  the workspace-scoped route.

`SessionSummary` includes a `parent_session_id` field (empty string for normal
sessions).

The compatibility `POST /api/sessions` response includes:

```json
{
  "session_id": "sid",
  "id": "sid",
  "workspace_hash": "abc123",
  "cwd": "C:/repo",
  "no_workspace": false
}
```

### `DELETE /api/sessions/:id`

Destroys an active in-memory session: aborts the current turn, joins the worker
thread, and removes it from the registry. It does not delete disk history.
Returns `204`; returns `503` when the session client is unavailable.

`DELETE /api/sessions/:id?purge=1` is the background-tasks "clear" action:
destroy plus permanent deletion of the session's disk data (`<id>.jsonl`,
`<id>.meta.json`, and the per-session `<id>/` directory holding persisted tool
results). Guard rails:

- `400 {"error":"only subagent sessions can be purged"}` when the target has no
  `parent_session_id` — main sessions cannot be purged through this path.
- `409 {"error":"session is busy; abort it first"}` while the sub-agent is
  running a turn.

### Archive, title, draft, and todos

Workspace-scoped and compatibility paths share the same behavior:

| Method | Path shape | Body | Response |
|---|---|---|---|
| PUT | `.../sessions/:id/archive` | ignored | updated `SessionSummary` |
| DELETE | `.../sessions/:id/archive` | none | updated `SessionSummary` |
| PUT | `.../sessions/:id/title` | `{"title":"..."}` | updated `SessionSummary` |
| GET | `.../sessions/:id/draft` | none | `{"session_id","id","text"}` |
| PUT | `.../sessions/:id/draft` | `{"text":"..."}` | `{"session_id","id","text"}` |
| DELETE | `.../sessions/:id/todos` | none | `{"session_id","id","workspace_hash","todos":[],"todo_summary":{...}}` |

Title writes trim whitespace and validate with `sanitize_title`.

### `GET /api/sessions/:id/messages?since=N`

When `since=0` or omitted, returns a full snapshot object:

```json
{
  "events": [],
  "messages": [],
  "busy": false,
  "turn_count": 4,
  "permission_mode": "default",
  "token_usage": null,
  "session_token_usage": null,
  "todos": [],
  "todo_summary": {},
  "goal": null
}
```

Hidden file checkpoints, compact checkpoints, and hidden goal context messages
are filtered from `messages`.

When `since>0`, returns an event array directly:

```json
[
  {"type":"message","seq":43,"timestamp_ms":1783152000000,"payload":{}}
]
```

If the requested sequence predates the in-memory replay ring, the array can be
empty. The frontend should fall back to `since=0`.

### `POST /api/sessions/:id/messages`

Queues a user input turn. Body:

```json
{
  "text": "Explain this code",
  "attachments": [{"id":"att-..."}],
  "contexts": [
    {
      "type": "selection",
      "label": "README.md:23-24",
      "text": "selected text",
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

`attachments` may contain strings or objects with an `id` field. `contexts`
with `type:"selection"` are sanitized and expanded into model-visible context
while preserving the user's original display text. Other context objects are
passed as browser context content parts.

If the text is a skill slash command for the session workspace, the daemon
expands it to the skill invocation prompt and records `metadata.display_text`.
Returns `202 {"queued":true}`.

### `POST /api/sessions/:id/attachments`

Uploads bytes into the session attachment store. Body:

```json
{
  "name": "screenshot.png",
  "mime_type": "image/png",
  "data_base64": "..."
}
```

Returns `201`:

```json
{
  "attachment": {
    "id": "att-...",
    "session_id": "sid",
    "name": "screenshot.png",
    "kind": "image",
    "mime_type": "image/png",
    "path": "...",
    "blob_url": "/api/sessions/sid/attachments/att-.../blob",
    "size_bytes": 12345
  }
}
```

### `GET /api/sessions/:id/attachments/:attachment_id/blob`

Returns raw attachment bytes with the stored MIME type and
`Cache-Control: private, max-age=3600`.

### `POST /api/sessions/:id/commands`

Runs daemon-owned builtin slash commands. Body:

```json
{"command":"compact","args":"","display_text":"/compact"}
```

`command` can also be slash text like `"/compact"`. Supported commands are
the daemon builtin commands accepted by `parse_builtin_command_request`:
`init`, `compact`, `goal`, and `plan`. Skill slash commands must use
`POST /api/sessions/:id/messages`.

Returns `202 {"queued":true,"command":"compact"}`. Errors:

- `400 {"error":"unsupported command","command":"..."}`
- `404 {"error":"unknown session"}`
- `500 {"error":"command failed"}`

### `GET /api/sessions/:id/permissions`

Returns the active or persisted session permission mode:

```json
{"mode":"default","description":"Prompt for write/exec tools"}
```

### `PUT /api/sessions/:id/permissions`

Body:

```json
{"mode":"yolo"}
```

Valid modes are `default`, `accept-edits`, `plan`, and `yolo`. Switching to
`yolo` also resolves any open permission prompt with allow. Returns the same
shape as `GET`.

### `POST /api/sessions/:id/fork`

Copies the source session prefix through `at_message_id` into a new session and
resumes it into the current daemon. It does not start a new turn.

Body:

```json
{"at_message_id":"msg-123","title":"optional title"}
```

Response:

```json
{
  "session_id": "new-sid",
  "title": "Fork title",
  "forked_from": "source-sid",
  "fork_message_id": "msg-123",
  "workspace_hash": "abc123",
  "cwd": "C:/repo",
  "no_workspace": false
}
```

### `POST /api/sessions/:id/file-checkpoints/:message_id/restore`

Restores workspace files to the checkpoint captured for that user turn. Chat
history is not rewound. Refuses while the session is busy.

Response:

```json
{
  "ok": true,
  "session_id": "sid",
  "message_id": "msg-123",
  "files_changed": 3,
  "errors": []
}
```

---

## 6. Opencode Import and Pins

### `GET /api/workspaces/:hash/opencode-import`

Previews importable opencode sessions:

```json
{
  "available": true,
  "count": 2,
  "source_database": "...",
  "error": "",
  "sessions": [
    {
      "id": "opencode-id",
      "title": "Title",
      "directory": "C:/repo",
      "provider": "openai",
      "model": "gpt-4.1",
      "archived": false,
      "time_created_ms": 0,
      "time_updated_ms": 0,
      "time_archived_ms": 0,
      "message_count": 10,
      "part_count": 20,
      "source_database": "..."
    }
  ]
}
```

### `POST /api/workspaces/:hash/opencode-import`

Starts an async import. Body is optional:

```json
{"session_ids":["opencode-id-1","opencode-id-2"]}
```

Returns `202` job status:

```json
{
  "job_id": "job-id",
  "workspace_hash": "abc123",
  "state": "pending",
  "imported": 0,
  "total": 2,
  "failed": 0,
  "skipped": 0,
  "current_title": "",
  "error": "",
  "session_ids": []
}
```

### `GET /api/workspaces/:hash/opencode-import/:job_id`

Polls the same status object. Returns `404` for unknown workspace or job.

### `GET /api/workspaces/:hash/pinned-sessions`

Returns:

```json
{"workspace_hash":"abc123","cwd":"C:/repo","session_ids":["sid-1"]}
```

The daemon prunes ids that no longer exist or are archived.

### `PUT /api/workspaces/:hash/pinned-sessions`

Body:

```json
{"session_ids":["sid-1","sid-2"]}
```

Normalizes, prunes, persists, and echoes the same shape as `GET`.

### `GET /api/pinned-sessions/order`

Returns cross-workspace ordering:

```json
{"items":[{"workspace_hash":"abc123","session_id":"sid-1"}]}
```

### `PUT /api/pinned-sessions/order`

Body:

```json
{"items":[{"workspace_hash":"abc123","session_id":"sid-1"}]}
```

Normalizes, prunes unavailable pinned items, persists, and echoes the same
shape as `GET`.

---

## 7. Files

All file routes validate `cwd` against the daemon cwd and registered workspace
cwds. `path` is relative to `cwd` and must stay within it.

### `GET /api/files?cwd=<abs>&path=<rel>&show_hidden=1`

Lists direct children:

```json
[
  {"name":"src","path":"src","kind":"directory","modified_ms":1783152000000},
  {"name":"README.md","path":"README.md","kind":"file","size":1234}
]
```

### `GET /api/files/content?cwd=<abs>&path=<rel>`

Returns `text/plain; charset=utf-8` file content. Error status examples:

- `400` unknown workspace or outside workspace
- `404` not found
- `415` binary or too large
- `500` IO error

### `GET /api/files/blob?cwd=<abs>&path=<rel>`

Returns raw bytes for browser-native preview types:

- images: `png`, `jpg`, `jpeg`, `gif`, `webp`, `bmp`, `ico`, `svg`
- documents: `pdf`, `docx`, `xlsx`, `xlsm`

The route caps preview bytes at 20 MB and sets `X-Content-Type-Options:
nosniff`.

---

## 8. Commands, Skills, and Hooks

### `GET /api/commands?workspace=<hash>`

Returns builtin slash commands and, when `workspace` is supplied, merged
workspace/global skills:

```json
{
  "builtins": [
    {"name":"init","description":"Analyze this codebase and generate (or improve) AGENT.md"},
    {"name":"compact","description":"Compress conversation history"},
    {"name":"goal","description":"Create, view, pause, resume, edit, or clear the thread goal"},
    {"name":"plan","description":"Enter plan mode or start planning a described task"}
  ],
  "skills": [
    {"name":"my-skill","description":"..."}
  ]
}
```

The `skills` field is omitted when no workspace cwd is provided.

### `GET /api/skills/root?workspace=<hash>`

Returns the effective skill directory:

```json
{
  "path": "C:/repo/.acecode/skills",
  "source": "project_acecode",
  "global_path": "C:/Users/me/.acecode/skills",
  "workspace_hash": "abc123",
  "cwd": "C:/repo"
}
```

`source` is `project_acecode`, `project_agent`, or `global_acecode`.
`global_path` always points at the user-global skills root
(`~/.acecode/skills`) regardless of which root was selected as `path`;
the settings page "open global skills directory" button relies on it.

### `GET /api/skills?workspace=<hash>`

Returns an array, not a wrapper:

```json
[
  {
    "name": "skill-name",
    "command_key": "/skill-name",
    "description": "...",
    "category": "custom",
    "enabled": true,
    "source": "project"
  }
]
```

The list is a full rescan of the workspace's project scan roots plus the
global scan roots, so disabled skills keep their real `description` and
`source`. `source` is `"project"` (discovered under the workspace's project
chain — `.acecode/skills` / `.agent/skills` walking up to, but not
including, HOME) or `"global"` (user-global roots and
`config.skills.external_dirs`). `workspace` is optional; without it the
daemon's compatibility workspace (its own cwd) is used.

Disabled config entries whose skill no longer exists on disk (ghost
entries) are still included with `enabled:false` and `source:""` so the UI
can release them from `config.skills.disabled`.

### `PUT /api/skills/:name?workspace=<hash>`

Body:

```json
{"enabled":false}
```

Returns `{"name":"skill-name","enabled":false}`.

`workspace` is optional and only affects the "known skill" validation: the
daemon's global registry only scans the daemon cwd's project chain, so
toggling a project skill that belongs to another workspace requires passing
that workspace's hash (the handler rescans that workspace's cwd to find the
skill). The disabled list itself is global config either way.

### `GET /api/skills/:name/body`

Returns `text/markdown; charset=utf-8` containing `SKILL.md`. Returns `404`
when the skill is not enabled/registered.

### Hook routes

| Method | Path | Behavior |
|---|---|---|
| GET | `/api/hooks` | returns current hook registry snapshot |
| POST | `/api/hooks/refresh` | reloads hook trust store and hook registry |
| POST | `/api/hooks/:id/trust` | persists trust for the hook definition |
| POST | `/api/hooks/:id/disable` | disables hook unless it is managed |
| POST | `/api/hooks/:id/enable` | enables hook |

Mutating hook routes return the refreshed hook registry snapshot. Managed hooks
cannot be disabled and return `409 {"error":"HOOK_MANAGED"}`.

---

## 9. Models and Copilot Auth

### `GET /api/models`

Returns an array of runtime-enabled saved model profiles. Editable fields such
as `base_url`, `api_key`, `request_headers`, `context_window`,
`stream_timeout_ms`, and `capabilities` are included when present.

### `POST /api/models`

Adds a saved model profile. Body is the saved model draft:

```json
{
  "name": "gateway-gpt",
  "provider": "openai",
  "model": "gpt-4.1",
  "base_url": "https://example.com/v1",
  "api_key": "{env:OPENAI_API_KEY}",
  "request_headers": {"X-Team":"acecode"},
  "context_window": 128000,
  "stream_timeout_ms": 600000,
  "capabilities": {}
}
```

Returns the saved profile. Validation errors use `BAD_JSON`, `BAD_REQUEST`, or
the `SavedModelEditError` string. Persistence failures roll back memory and
return `500 PERSIST_FAILED`.

### `PUT /api/models/:name`

Updates a saved model profile and may rename it. Missing `api_key`,
`base_url`, `context_window`, `stream_timeout_ms`, `capabilities`, or
`request_headers` preserve the existing values. Sending empty
`request_headers` clears them. Returns the updated profile.

### `DELETE /api/models/:name`

Removes a saved model profile. If a busy active session is using the profile,
returns `409 MODEL_IN_USE`. On success:

```json
{"ok":true}
```

### `POST /api/models/probe`

Probes provider model ids. OpenAI-compatible providers call upstream
`GET /models`; Copilot uses saved GitHub auth. Anthropic model ids are entered
manually and are not probed.

Success:

```json
{
  "models": ["gpt-4.1"],
  "model_context_windows": {"gpt-4.1": 1047576}
}
```

Errors include `COPILOT_AUTH_REQUIRED`, `INVALID_REQUEST_HEADER`,
`PROBE_FAILED`, `PROBE_HTTP_ERROR`, and `PROBE_BAD_JSON`.

### `GET /api/config/default-model`

Returns:

```json
{"name":"saved-model-name"}
```

### `POST /api/config/default-model`

Body:

```json
{"name":"saved-model-name"}
```

The name must exist in `saved_models`. Success returns:

```json
{"default_model_name":"saved-model-name"}
```

### `GET /api/sessions/:id/model?workspace=<hash>`

Returns current session model state:

```json
{
  "name": "saved-model-name",
  "provider": "openai",
  "model": "gpt-4.1",
  "context_window": 128000,
  "deleted": false
}
```

### `POST /api/sessions/:id/model`

Body:

```json
{"name":"saved-model-name"}
```

Switches the active session to that saved model profile and returns model
state. Returns `404` when the session is not active in the registry.

### Copilot auth routes

| Method | Path | Response |
|---|---|---|
| GET | `/api/copilot/auth` | `{"provider":"copilot","has_token":true,"authenticated":true}` |
| DELETE | `/api/copilot/auth` | deletes saved GitHub token, returns auth false |
| POST | `/api/copilot/auth/device` | starts GitHub device flow |
| POST | `/api/copilot/auth/device/poll` | polls one device-flow tick |

`POST /api/copilot/auth/device` response:

```json
{
  "status": "pending",
  "provider": "copilot",
  "device_code": "...",
  "user_code": "ABCD-1234",
  "verification_uri": "https://github.com/login/device",
  "interval": 5,
  "expires_in": 900,
  "expires_at_unix_ms": 1783152000000
}
```

Polling success returns `status:"authenticated"`. Pending, slow-down, and
failure states return `status`, `error`, `message`, and
`interval_delta_seconds`.

---

## 10. Config, MCP, Update, and Feedback

### `GET /api/config/ui-preferences`

Returns:

```json
{"show_acecode_avatar":false}
```

The avatar preference is kept for compatibility and is always normalized to
`false`.

### `PUT /api/config/ui-preferences`

Body:

```json
{"show_acecode_avatar":false}
```

Validates the field, persists config, and echoes the normalized response.

### `GET /api/config/custom-instructions`

Returns:

```json
{"text":"custom prompt text"}
```

### `PUT /api/config/custom-instructions`

Body:

```json
{"text":"custom prompt text"}
```

The text is byte-limited by `kCustomInstructionsMaxBytes`. Existing sessions
pick up changes on later turns through the daemon config pointer.

### `GET /api/config/connectors`

Returns:

```json
{"connectors":[]}
```

### `PUT /api/config/connectors`

Body:

```json
{"connectors":[]}
```

Parses connector config, persists, and echoes `{"connectors":[...]}`.

### `GET /api/config/default-permission-mode`

Returns the permission mode used by newly-created sessions:

```json
{"mode":"accept-edits","description":"Auto-allow file edits, prompt for bash"}
```

### `PUT /api/config/default-permission-mode`

Body:

```json
{"mode":"accept-edits"}
```

Persists the default and updates the in-memory session registry default.

### `GET /api/config/upgrade`

Returns:

```json
{"base_url":"https://example.com/acecode"}
```

### `PUT /api/config/upgrade`

Body:

```json
{"base_url":"https://example.com/acecode"}
```

Normalizes and validates a non-empty HTTP(S) base URL.

### `GET /api/update/status`

Checks the update manifest and returns:

```json
{
  "status": "ok",
  "update_available": true,
  "current_version": "0.5.10",
  "latest_version": "0.5.11",
  "target": "windows-x64",
  "manifest_url": "https://example.com/manifest.json",
  "package_file": "acecode.zip",
  "package_url": "https://example.com/acecode.zip",
  "package_size": 123456
}
```

`http_status` and `error` are included when present.

### `POST /api/update/start`

Checks for an update and starts `acecode update` or a desktop-provided update
command. Returns `202`:

```json
{"started":true,"latest_version":"0.5.11","message":"acecode update started"}
```

Returns `409 NO_UPDATE` when no compatible update is available.

### `GET /api/config/ace-browser-bridge`

Returns browser bridge tool settings:

```json
{
  "enabled": true,
  "tool_mode": "native",
  "default_mode": "auto",
  "pointer_speed": 1.0,
  "status_cache_ttl_ms": 1000,
  "tool_timeout_ms": 60000,
  "os_pointer_enabled": true,
  "tab_group_enabled": true,
  "operation_overlay_enabled": true,
  "operation_overlay_watchdog_ms": 10000,
  "pointer_custom": {
    "move_duration_ms_min": 80,
    "move_duration_ms_max": 240,
    "click_hold_ms_min": 40,
    "click_hold_ms_max": 120,
    "typing_delay_ms_min": 0,
    "typing_delay_ms_max": 20,
    "jitter_px": 1,
    "max_path_points": 48
  }
}
```

### `PUT /api/config/ace-browser-bridge`

Body:

```json
{"enabled":true}
```

Persists `enabled`, unregisters existing bridge tools, and registers them again
when enabled. The response is the full bridge settings object.

### `GET /api/mcp`

Reads `mcp_servers` from config. `auth_token` is intentionally not returned.

```json
{
  "server-name": {
    "transport": "stdio",
    "command": "node",
    "args": ["server.js"],
    "env": {},
    "url": "",
    "sse_endpoint": "/sse",
    "headers": {},
    "timeout_seconds": 30
  }
}
```

### `PUT /api/mcp`

Overwrites `mcp_servers`. Body is an object keyed by server name. Success:

```json
{"saved":true,"reload_required":true}
```

### `POST /api/mcp/reload`

Currently returns `501`:

```json
{"error":"mcp reload not implemented in v1; restart daemon to pick up changes"}
```

### `GET /api/feedback/desktop/recent-sessions?limit=N`

Returns recent sessions for optional feedback attachment. `limit` defaults to
`20` and is clamped to `1..100`.

```json
{"sessions":[{"id":"sid","session_id":"sid","title":"...","workspace_hash":"abc123"}]}
```

### `POST /api/feedback/desktop`

Body fields are optional strings:

```json
{
  "feedback_text": "Settings page froze",
  "session_id": "sid",
  "workspace_hash": "abc123"
}
```

If `session_id` is empty, the package contains desktop logs only. The upload
target is derived from `upgrade.base_url`.

Success:

```json
{
  "ok": true,
  "package_filename": "acecode-feedback-desktop-....zip",
  "log_included": true,
  "log_tail_bytes": 4312,
  "included_files": ["logs/desktop.log.tail.txt","feedback.json"],
  "selected_session_id": null,
  "workspace_hash": ""
}
```

Errors include `SESSION_NOT_FOUND`, `PACKAGE_FAILED`, and `UPLOAD_FAILED`.

---

## 11. Console PTY

PTY endpoints are loopback-only because they execute shell input without the
agent tool permission gate. Non-loopback requests return `403` even with a
token.

### `GET /api/pty/shells`

Returns detected shell choices and the configured default:

```json
{
  "shells": [
    {"id":"powershell","label":"PowerShell","available":true,"needs_path":false}
  ],
  "default": "powershell"
}
```

### `PUT /api/console/config`

Body:

```json
{"default_shell":"powershell","git_bash_path":"C:/Program Files/Git/bin/bash.exe"}
```

Both fields are optional. `git_bash_path` is trimmed, dequoted, checked for WSL
System32 bash, and validated if non-empty. Returns the same payload as
`GET /api/pty/shells`.

### `POST /api/pty`

Body:

```json
{"cwd":"C:/repo","title":"Terminal","shell":"powershell"}
```

`shell` is a shell id from `/api/pty/shells`. The daemon enforces a 16-session
limit and returns `429` when exceeded.

Session info:

```json
{
  "id": "pty-1",
  "title": "Terminal 1",
  "shell": "C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
  "cwd": "C:/repo",
  "status": "running",
  "pid": 12345,
  "backend": "conpty",
  "exit_code": 0
}
```

`exit_code` appears only when `status == "exited"`.

### `GET /api/pty`

Returns:

```json
{"backend":"conpty","sessions":[]}
```

### `DELETE /api/pty/:id`

Kills/removes the PTY session. Returns `204` or `404`.

### `POST /api/pty/:id/resize`

Body:

```json
{"cols":120,"rows":30}
```

`cols` and `rows` must be in `2..1000`. Returns `204`.

### `POST /api/pty/:id/title`

Body:

```json
{"title":"npm run dev"}
```

Used by the frontend to persist xterm OSC title changes. Returns `204`.

---

## 12. Session WebSocket

Route:

```text
WS /ws/sessions/:route?token=<token>
```

The frontend currently connects to `/ws/sessions/_multiplex`. The route
parameter is not the session id; sessions are bound by JSON messages after the
socket opens.

### Server event envelope

Session events are JSON objects:

```json
{
  "type": "token",
  "seq": 1,
  "timestamp_ms": 1783152000000,
  "session_id": "sid",
  "workspace_hash": "abc123",
  "payload": {}
}
```

Most session event frames include a per-session `seq` and `timestamp_ms`.
One exception is the `permission_request` snapshot frame that can be sent
immediately after `subscribe_ack` when a permission request is already pending;
that replay frame intentionally has no `seq` so clients do not advance or warn
on the reconnect cursor. Clients should de-duplicate permission prompts by
`payload.request_id`.

Session event `type` values from `SessionEventKind`:

- `token`
- `reasoning`
- `agent_progress`
- `message`
- `tool_start`
- `tool_update`
- `tool_end`
- `permission_request`
- `question_request`
- `question_closed`
- `usage`
- `transcript_replace`
- `goal_updated`
- `goal_cleared`
- `todo_updated`
- `session_updated`
- `busy_changed`
- `done`
- `error`

`transcript_replace` is for retry/recovery cleanup. Normal compact success
appends visible marker messages and a hidden checkpoint instead.

### Client messages

All client frames are JSON:

```json
{"type":"subscribe","payload":{"session_id":"sid","since":42}}
```

| Type | Payload | Behavior |
|---|---|---|
| `hello` | `{session_id,since}` | legacy bind; ack is `hello_ack` |
| `subscribe` | `{session_id,since}` | subscribes one session; ack is `subscribe_ack`; may then send seq-less pending `permission_request` snapshots |
| `unsubscribe` | `{session_id}` | unsubscribes; ack is `unsubscribe_ack` |
| `status_subscribe` | `{workspace_hash}` or `{session_id}` | subscribes workspace attention status and sends snapshot |
| `status_unsubscribe` | `{workspace_hash}` | unsubscribes; ack is `status_unsubscribe_ack` |
| `mark_session_read` | `{session_id,workspace_hash,cursor}` | persists read cursor; ack is `mark_session_read_ack` |
| `user_input` | `{session_id,text}` | queues plain user input |
| `decision` | `{session_id,request_id,choice}` | responds to permission request; `choice` is `allow`, `deny`, or `allow_session` |
| `question_answer` | `{session_id,request_id,cancelled,answers}` | responds to AskUserQuestion |
| `abort` | `{session_id}` | aborts current turn |
| `ping` | `{}` | replies `{"type":"pong"}` |

`decision` uses `choice`, not `decision`, in the payload.

`question_answer.answers[]` entries are:

```json
{
  "question_id": "q1",
  "selected": ["option-id"],
  "custom_text": "free form"
}
```

When more than one session is subscribed, session-targeted messages should
include `payload.session_id`.

### Acks and status messages

Subscribe ack:

```json
{
  "type": "subscribe_ack",
  "session_id": "sid",
  "workspace_hash": "abc123",
  "payload": {"session_id":"sid","workspace_hash":"abc123","cwd":"C:/repo"}
}
```

Workspace status snapshot:

```json
{
  "type": "session_status_snapshot",
  "timestamp_ms": 1783152000000,
  "workspace_hash": "abc123",
  "payload": {
    "workspace_hash": "abc123",
    "sessions": [
      {
        "session_id": "sid",
        "workspace_hash": "abc123",
        "cwd": "C:/repo",
        "state": "read",
        "attention_state": "read",
        "read_state": "read",
        "busy": false,
        "cursor": 0,
        "update_cursor": 0,
        "read_cursor": 0
      }
    ]
  }
}
```

Live attention updates use `type:"session_status"` with the same payload shape
for one session.

### Reconnect strategy

1. Store the highest processed `seq` per session.
2. Reconnect and send `subscribe` with `since:<lastSeq>`.
3. The daemon replays buffered events with `seq > since`.
4. If the replay gap is too old, fall back to
   `GET /api/sessions/:id/messages?since=0`.

Seq-less pending `permission_request` snapshots do not affect the reconnect
cursor; handle them by `request_id`.

---

## 13. PTY WebSocket

Route:

```text
WS /ws/pty/:id?cursor=N&token=<token>
```

This socket is loopback-only. Unlike the session socket, it is a raw byte
transport, not JSON envelopes.

Server to client:

- Binary frames are PTY output bytes.
- Frames whose first byte is `0x00` are UTF-8 JSON control frames, such as
  `{"cursor":123}` after backlog replay or `{"exit_code":0}` on exit.

Client to server:

- Text or binary frames are written verbatim to PTY stdin.

Each PTY session keeps a 2 MB rolling output buffer with a monotonic byte
cursor. `cursor=N` replays from that offset; `cursor=-1` skips backlog.
Resize uses `POST /api/pty/:id/resize`, not the WebSocket.

---

## 14. Static Web App

The daemon also serves the built frontend:

- `GET /` serves the SPA entry.
- `GET /<path>` up to four path segments serves static files or falls back to
  the SPA entry.
- `/api/*` and `/ws/*` never fall back to the SPA; unmatched API/WS paths are
  `404`.

---

## 15. HTTP Status Summary

| Status | Meaning |
|---|---|
| 200 | OK |
| 201 | Created |
| 202 | Accepted/queued |
| 204 | No content |
| 400 | Bad JSON, missing field, validation failure |
| 401 | Missing or bad daemon token |
| 403 | PTY non-loopback access |
| 404 | Unknown route, workspace, session, attachment, skill, or job |
| 409 | Conflict, busy session, active writer, no update, model in use |
| 415 | File preview unsupported, binary, or too large |
| 429 | PTY session limit |
| 500 | Persistence, command, package, restore, or internal failure |
| 501 | Feature unavailable or not implemented |
| 502 | Upstream probe, upload, or auth exchange failure |
| 503 | Required daemon subsystem unavailable |

---

## 16. Process Exit Codes

| rc | Where | Meaning |
|---|---|---|
| 0 | any | Normal exit |
| 1 | various CLI | Generic failure |
| 2 | `worker.cpp` | preflight bind check rejected |
| 3 | `worker.cpp` / `server.cpp` | Crow `app.run()` failed, usually port in use |
| 4 | `worker.cpp` | failed to write runtime file |
| 5 | `cli.cpp foreground` | config validation failed |
| 6 | `cli.cpp start` | another daemon already running |
| 7 | `cli.cpp start` | detached spawn failed |
| 8 | `cli.cpp start` | detached worker did not write pid in time |
| 9 | `cli.cpp stop` | terminate pid did not complete |
| 10 | `cli.cpp` / `service_win.cpp` | unknown subcommand |
| 11 | `cli.cpp` / `service_win.cpp` | missing subcommand |
| 21 | `service_win.cpp` | `--service-main` invoked outside SCM |
| 22 | `service_win.cpp` | `StartServiceCtrlDispatcher` failed |
| 24 | `service_win.cpp` | access denied, admin required |
| 25-33 | `service_win.cpp` | other SCM API failures |
| 64 | `main.cpp` | `--service-main` on non-Windows |
| 65 | `main.cpp` | `service` subcommand on non-Windows |
