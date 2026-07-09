## Context

`AgentLoop` already emits `on_busy_changed(true)` at the start of model, compact, and shell turns, and `on_busy_changed(false)` when those turns finish. TUI owns one local loop. Daemon owns a `SessionRegistry` containing multiple loops, including web/desktop sessions and subagent sessions.

The desired behavior is process-wide: if any loop is busy, ACECode should hold one OS-level inhibitor. The inhibitor should not be tied to the active UI tab or focused session.

## Goals / Non-Goals

**Goals:**

- Prevent sleep/display lock while any ACECode session in the process is busy.
- Make TUI and daemon-created sessions use the same small API.
- Support Windows, macOS, and Linux without adding third-party dependencies.
- Keep inhibition best-effort and non-blocking for agent turns.

**Non-Goals:**

- Do not add UI controls or settings in this change.
- Do not inhibit sleep while ACECode is idle.
- Do not coordinate across unrelated ACECode processes. Each process manages its own sessions.
- Do not guarantee every Linux desktop environment supports inhibition; use best effort.

## Decisions

### Decision 1: Use a process-wide busy-session registry

Introduce a reusable `ActiveSessionPowerGuard` that tracks busy session IDs in a set. The first busy session acquires the OS inhibitor; the last idle transition releases it.

Alternative considered: poll `SessionRegistry::list_active()` periodically. The callback path is cheaper, immediate, and also works for TUI where no registry owns multiple sessions.

### Decision 2: Native/simple platform backends

Use `SetThreadExecutionState` on Windows. On macOS, launch `/usr/bin/caffeinate -dims -w <pid>` and terminate it on release. On Linux, prefer `systemd-inhibit --what=idle:sleep --mode=block --who=ACECode --why=... sleep infinity`; when unavailable, fall back to no-op with a diagnostic.

Alternative considered: DBus desktop inhibition on Linux. That would cover more desktop environments but requires more integration code and runtime dependencies; `systemd-inhibit` is a good first best-effort path for this C++ CLI/daemon app.

### Decision 3: Hook existing busy callbacks

TUI wraps its existing `on_busy_changed` callbacks with the guard. Daemon installs the guard at session creation so every daemon-backed loop, including subagents, contributes to the same process-level inhibitor.

Alternative considered: add power management inside `AgentLoop`. Keeping it outside the loop avoids hard-coding process policy into core agent execution and keeps tests easier.

## Risks / Trade-offs

- [Risk] Linux systems without `systemd-inhibit` will not be protected. -> Mitigation: keep the API best-effort and expose the implementation boundary so a future DBus backend can be added.
- [Risk] A missing idle transition could leave inhibition active. -> Mitigation: session destruction/shutdown should report idle or release all active entries, and the OS releases process-owned handles on exit.
- [Risk] macOS helper process cleanup might fail. -> Mitigation: terminate the helper on release and rely on `-w <pid>` so it exits if ACECode exits.
