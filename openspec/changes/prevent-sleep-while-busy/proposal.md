## Why

Long ACECode turns can run unattended while the user waits for model reasoning, tool calls, or background subagents. Today the operating system may still lock the screen or put the display/session to sleep during that work, interrupting monitoring and making desktop/web users think ACECode has stalled.

## What Changes

- Keep the host awake while at least one ACECode session in the current process is busy.
- Share the busy-session tracking across TUI, daemon, web, desktop-backed daemon sessions, and spawned subagent sessions.
- Add a reusable cross-platform power inhibitor with native Windows support, native macOS support, and Linux best-effort support.
- Release the inhibitor as soon as the last busy session becomes idle, and also during process shutdown.
- Keep failures non-fatal: ACECode should continue even when the current platform cannot install an inhibitor.

## Capabilities

### New Capabilities

- `power-management`: ACECode prevents system sleep/display lock while agent work is in progress.

### Modified Capabilities

- `session-runtime`: Busy/idle transitions now also drive process-level power inhibition.

## Impact

- Affected C++ areas: a new power-management utility, `src/main.cpp`, `src/tui/agent_callbacks_builder.cpp`, and daemon session creation/status wiring.
- Affected surfaces: TUI, daemon API/web UI, desktop shell through the supervised daemon, and background subagents.
- Affected tests: focused unit tests for the busy-session tracker and platform command construction where practical.
- No user configuration or protocol changes are required for the initial behavior.
