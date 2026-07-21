# Hooks

ACECode supports Codex-compatible lifecycle hooks for local automation. Hooks are local commands that receive event JSON on stdin and may return structured output to add context, block prompts or tools, approve permissions, stop compact, or continue a turn.

Reference behavior follows the OpenAI Codex hooks documentation: <https://developers.openai.com/codex/hooks>.

## Enable Or Disable

Codex-compatible hooks are enabled by default.

```json
{
  "features": {
    "hooks": false
  }
}
```

When `features.hooks` is `false`, ACECode skips Codex-compatible hook discovery and dispatch. Existing ACECode legacy startup hooks still keep their compatibility behavior.

## Config Locations

ACECode discovers hook files in this order and merges definitions from every loaded source:

1. User-global ACECode legacy hooks: `~/.acecode/hooks.json`
2. User-global Codex hooks: `~/.codex/hooks.json`
3. Project ACECode hooks: `<workspace>/.acecode/hooks.json`
4. Project Codex hooks: `<workspace>/.codex/hooks.json`

Project-local hook files are loaded only for the active trusted workspace. Invalid or missing files produce diagnostics but do not stop ACECode.

## Codex-Compatible Shape

Codex-compatible hook files use an event object, matcher groups, and handlers:

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {
            "type": "command",
            "command": "python ./hooks/check_tool.py",
            "commandWindows": "py .\\hooks\\check_tool.py",
            "timeout": 30,
            "statusMessage": "Checking tool request"
          }
        ]
      }
    ]
  }
}
```

Command handlers run through the platform shell. `commandWindows` or `command_windows` overrides `command` on Windows. `timeout` is seconds and defaults to `600`.

Unsupported `prompt` and `agent` handlers are parsed as skipped hooks with diagnostics. `async: true` Codex command hooks are also skipped.

## Trust Review

Non-managed command hooks are local code and require review before they run. ACECode stores trust and disabled state in the local ACECode data directory, not in the project.

- New non-managed hooks are `pending_review` and skipped.
- Trusted hooks run until their definition hash changes.
- Changed hooks return to `pending_review`.
- Disabled hooks are skipped even if trusted.
- Managed hooks are trusted by policy and cannot be disabled from the hook browser.

Use the desktop/web Settings page, section `钩子`, to view sources, commands, diagnostics, hashes, and trust state. The page can refresh hook discovery, trust pending hooks, disable non-managed hooks, and re-enable disabled hooks.

The daemon API exposes the same management surface:

- `GET /api/hooks`
- `POST /api/hooks/refresh`
- `POST /api/hooks/<encoded-hook-id>/trust`
- `POST /api/hooks/<encoded-hook-id>/disable`
- `POST /api/hooks/<encoded-hook-id>/enable`

Hook ids may contain path separators, so API clients must URL-encode them.

## Matchers

An omitted matcher, an empty matcher, or `*` matches all events in that group. Other matcher values are regular expressions evaluated against the event-specific target.

Tool events match the canonical tool name plus Codex aliases:

- `Bash` matches ACECode shell tool calls.
- `apply_patch`, `Edit`, and `Write` match supported file edit and write paths.
- MCP tools match their registered tool names.

Compact hooks match `manual` or `auto`. `SessionStart` hooks match `startup`, `resume`, `clear`, or `compact`.

## Common Input

Codex-compatible command hooks receive UTF-8 JSON on stdin. Common fields include:

```json
{
  "session_id": "session-id",
  "transcript_path": "C:/path/to/session.jsonl",
  "cwd": "C:/repo",
  "hook_event_name": "PreToolUse",
  "model": "gpt-4.1",
  "permission_mode": "default",
  "turn_id": "turn-id"
}
```

`permission_mode` appears on session, prompt, tool, permission, and stop hooks. `turn_id` appears on turn-scoped hooks.

## Outputs

Exit code `0` with empty stdout means success with no behavioral effect. JSON stdout may include:

- `continue`
- `stopReason`
- `systemMessage`
- `decision`
- `hookSpecificOutput`

Exit code `2` uses stderr as an event-specific block, feedback, or continuation reason when the event supports it.

Unsupported output fields are diagnostic failures and fall back to the event's default behavior.

## Supported Events

### `SessionStart`

Runs when a session starts, resumes, clears, or is recreated after compact. Input includes `source`.

Plain text stdout or `hookSpecificOutput.additionalContext` is added to the next model request as request-local context. It is not persisted to transcript history.

```json
{
  "hooks": {
    "SessionStart": [
      {
        "matcher": "startup|resume",
        "hooks": [
          { "type": "command", "command": "python ./hooks/session_context.py" }
        ]
      }
    ]
  }
}
```

### `UserPromptSubmit`

Runs before a user prompt is persisted or sent to the model. Input includes `prompt`.

Returning `{"decision":"block","reason":"..."}` or exiting with code `2` blocks submission. Additional context is added to the current turn when not blocked.

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          { "type": "command", "command": "python ./hooks/check_prompt.py" }
        ]
      }
    ]
  }
}
```

### `PreToolUse`

Runs before supported tool execution and before normal permission prompts. Input includes `tool_name`, `tool_use_id`, and `tool_input`.

Returning `permissionDecision: "deny"` or `decision: "block"` skips the tool and returns the hook reason as the tool result. Returning valid `updatedInput` lets supported tools run with rewritten input.

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          { "type": "command", "command": "python ./hooks/deny_shell.py" }
        ]
      }
    ]
  }
}
```

### `PermissionRequest`

Runs only when ACECode would otherwise ask the user for permission. Deny wins over allow. Allow skips the normal prompt. No decision preserves normal approval behavior.

```json
{
  "hooks": {
    "PermissionRequest": [
      {
        "matcher": "Write|Edit|apply_patch",
        "hooks": [
          { "type": "command", "command": "python ./hooks/permission_policy.py" }
        ]
      }
    ]
  }
}
```

### `PostToolUse`

Runs after supported tools complete. Input includes `tool_response`.

Returning `decision: "block"` or `continue: false` cannot undo side effects, but it can replace the model-visible tool result with hook feedback.

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          { "type": "command", "command": "python ./hooks/rewrite_feedback.py" }
        ]
      }
    ]
  }
}
```

### `PreCompact` And `PostCompact`

`PreCompact` runs once before an actual manual or automatic compact attempt and can stop it with `continue: false`. `PostCompact` runs once after a successful compact checkpoint has installed replacement model history; compaction never replaces the append-only human transcript. A failed compact does not run `PostCompact` and does not install a checkpoint.

```json
{
  "hooks": {
    "PreCompact": [
      {
        "matcher": "manual",
        "hooks": [
          { "type": "command", "command": "python ./hooks/check_compact.py" }
        ]
      }
    ]
  }
}
```

### `Stop`

Runs before an agent turn returns control. Input includes `stop_hook_active` and `last_assistant_message`.

Returning `decision: "block"` with a reason continues the agent loop with an internal user prompt. ACECode sets `stop_hook_active` to prevent unbounded stop-hook loops. `continue: false` takes precedence over continuation.

```json
{
  "hooks": {
    "Stop": [
      {
        "hooks": [
          { "type": "command", "command": "python ./hooks/final_check.py" }
        ]
      }
    ]
  }
}
```

## ACECode Legacy Hooks

The original ACECode schema still works for compatibility:

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
        "command": "python",
        "args": ["C:\\hooks\\models_loaded.py"],
        "timeout_ms": 3000
      }
    ],
    "assistant.message_completed": [
      {
        "id": "assistant-done",
        "mode": "async",
        "command": "python",
        "args": ["C:\\hooks\\assistant_done.py"]
      }
    ]
  }
}
```

Legacy hooks use direct `command + args` process execution, not shell command strings. Their stdout and stderr remain diagnostic-only and do not modify ACECode runtime behavior. Existing enabled user-global legacy hooks are auto-trusted for compatibility.

## Limitations

- `SubagentStart` and `SubagentStop` are deferred until ACECode has a first-class subagent lifecycle.
- `prompt` and `agent` handlers are parsed but skipped.
- `async: true` Codex command handlers are parsed but skipped.
- Hooks are not a sandbox. They run as local user code after trust review.
- `PreToolUse` covers the normal agent tool path and supported manual shell `!cmd` paths, but future tools that bypass ACECode's tool executor need explicit wiring.
- Manual shell `!cmd` has `PreToolUse` and `PostToolUse` coverage where feasible, but no `PermissionRequest` event because that path does not show the normal approval prompt.
