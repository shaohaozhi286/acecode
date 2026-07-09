### Requirement: Busy sessions inhibit system sleep

ACECode SHALL request an operating-system power inhibitor while at least one session in the current ACECode process is busy.

#### Scenario: First busy session acquires inhibitor

- **GIVEN** no session is busy
- **WHEN** any ACECode session emits `busy=true`
- **THEN** ACECode SHALL acquire the process power inhibitor

#### Scenario: Multiple busy sessions share one inhibitor

- **GIVEN** one session is already busy and the inhibitor is active
- **WHEN** another session emits `busy=true`
- **THEN** ACECode SHALL keep the existing inhibitor active without requiring a second OS inhibitor

#### Scenario: Last idle session releases inhibitor

- **GIVEN** multiple sessions have emitted `busy=true`
- **WHEN** every tracked busy session emits `busy=false`
- **THEN** ACECode SHALL release the process power inhibitor

### Requirement: Three-platform best-effort support

ACECode SHALL provide platform-specific inhibition for Windows, macOS, and Linux without adding external library dependencies.

#### Scenario: Platform backend is unavailable

- **GIVEN** the current platform cannot install a sleep inhibitor
- **WHEN** a session emits `busy=true`
- **THEN** ACECode SHALL continue running the session
- **AND** the failed inhibitor attempt SHALL NOT block model reasoning or tool execution

### Requirement: All ACECode surfaces contribute to inhibition

TUI sessions, daemon/web sessions, desktop-backed daemon sessions, and daemon subagent sessions SHALL all contribute busy/idle transitions to the same process-level busy-session tracker.

#### Scenario: Background session keeps host awake

- **GIVEN** the active UI is viewing an idle session
- **AND** a different daemon session or subagent is busy
- **THEN** ACECode SHALL keep the power inhibitor active until that background session becomes idle
