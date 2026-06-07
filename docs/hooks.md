# Hooks

ACECode can run user-defined local hook commands for selected lifecycle events. Hooks are configured in a separate file:

```text
~/.acecode/hooks.json
```

If the file is missing, hooks are disabled. The first version only supports user-global hooks from ACECode home; project-local hooks are not loaded.

## Configuration

```json
{
  "version": 1,
  "enabled": true,
  "events": {
    "startup.before_model_load": [
      {
        "id": "refresh-model-config",
        "mode": "sync",
        "platforms": ["windows"],
        "command": "C:\\hooks\\refresh-model-config.exe",
        "args": [],
        "timeout_ms": 0
      }
    ],
    "startup.models_loaded": [
      {
        "id": "models-loaded",
        "mode": "sync",
        "platforms": ["windows"],
        "command": "python",
        "args": ["C:\\hooks\\models_loaded.py"],
        "timeout_ms": 3000
      }
    ],
    "assistant.message_completed": [
      {
        "id": "assistant-done",
        "mode": "async",
        "platforms": ["windows", "posix"],
        "commands": {
          "windows": {
            "command": "python",
            "args": ["C:\\hooks\\assistant_done.py"]
          },
          "posix": {
            "command": "python3",
            "args": ["/home/you/hooks/assistant_done.py"]
          }
        },
        "timeout_ms": 0
      }
    ]
  }
}
```

Each hook is launched as `command` plus `args`; ACECode does not run hook definitions through a shell and does not expand `~`, `$HOME`, `%USERPROFILE%`, pipes, or redirects. Absolute commands are used as-is. Relative commands are first resolved against the directory containing the running ACECode executable when a matching file exists there; otherwise bare command names are left for the operating system's normal PATH lookup. The event payload is written to the process as UTF-8 JSON on stdin. Hook stdout and stderr are captured for logs only and do not modify ACECode state.

## Events

### `startup.before_model_load`

Dispatched before ACECode reads `~/.acecode/config.json` for the interactive TUI, daemon foreground worker, and Windows service startup paths. Use this event for local automation that must update model configuration before the current process resolves the active model.

If the hook must affect the current startup, configure it with `mode: "sync"`. An `async` hook may still be running when ACECode reads model configuration.

Payload includes:

- `schema_version`
- `event`
- `hook_id`
- `timestamp`
- `process.pid`
- `process.cwd`
- `config.path`

### `startup.models_loaded`

Dispatched after the local model registry and active model profile have been resolved during startup. This does not mean ACECode has fetched a remote provider model list.

Payload includes:

- `schema_version`
- `event`
- `hook_id`
- `timestamp`
- `process.pid`
- `process.cwd`
- `models.registry_source`
- `models.active`

### `assistant.message_completed`

Dispatched after a successful assistant message is committed. For SSE providers, this happens after the stream returns `Done` and ACECode has built the assistant message. Provider errors, aborts, and turns without a committed assistant message do not dispatch this event.

Payload includes:

- `schema_version`
- `event`
- `hook_id`
- `timestamp`
- `session.id`
- `session.cwd`
- `model.provider`
- `model.model`
- `assistant.kind`, either `text` or `tool_calls`
- `assistant.content`
- `assistant.tool_calls`

## Modes

- `sync`: ACECode waits for the hook to finish or time out before continuing the triggering flow.
- `async`: ACECode enqueues the hook for a background worker and continues the triggering flow immediately.

Async hooks use a single background worker in the first version. An async hook with `timeout_ms <= 0` can occupy that worker until it exits.

## Timeout

- `timeout_ms > 0`: ACECode applies that timeout to the hook process.
- `timeout_ms <= 0`: ACECode waits without a hook-specific timeout.

Failures, non-zero exit codes, and timeouts are logged but do not fail startup or chat.

## Platforms

Supported platform names:

- `windows`
- `linux`
- `mac`
- `unix`
- `posix`

Linux matches `linux`, `unix`, and `posix`. macOS matches `mac`, `unix`, and `posix`. Windows matches `windows`.

When `commands` contains multiple matching entries, resolution order is:

- Windows: `windows`
- Linux: `linux`, then `unix`, then `posix`
- macOS: `mac`, then `unix`, then `posix`

If `platforms` is omitted, the hook can run on any platform as long as ACECode can resolve a command for the current platform.
