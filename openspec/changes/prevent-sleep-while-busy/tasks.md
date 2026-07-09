## 1. Power Inhibitor Utility

- [x] 1.1 Add a reusable process-level power inhibitor interface and active-session tracker.
- [x] 1.2 Implement Windows, macOS, and Linux best-effort backends without third-party dependencies.
- [x] 1.3 Make release idempotent and safe during shutdown or repeated busy/idle transitions.

## 2. Runtime Integration

- [x] 2.1 Wire TUI `on_busy_changed` transitions into the process power guard.
- [x] 2.2 Wire daemon-created sessions and subagent sessions into the same guard.
- [x] 2.3 Ensure session removal/shutdown releases any tracked busy session.

## 3. Regression Coverage

- [x] 3.1 Add focused tests for active-session counting and first-acquire/last-release behavior.
- [x] 3.2 Add tests for duplicate busy/idle transitions and release-all cleanup.

## 4. Verification

- [x] 4.1 Run focused C++ unit tests for the new utility.
- [x] 4.2 Run broader unit/build checks that cover TUI/daemon compilation.
- [ ] 4.3 Run OpenSpec validation if the `openspec` CLI is available. (Skipped: `openspec` is not installed in this environment.)
