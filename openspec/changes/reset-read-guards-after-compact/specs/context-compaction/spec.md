## ADDED Requirements

### Requirement: Compact invalidates model-history-dependent read guards
The system SHALL invalidate runtime guard/cache state that relies on pre-compact AI-facing history whenever a compact operation successfully installs a replacement AI-facing history. It MUST preserve independent edit-safety baselines used to validate later file edits.

#### Scenario: Manual compact resets read-observation cache
- **WHEN** a session reads a file and then a manual compact successfully installs a replacement AI-facing history
- **THEN** the next `file_read` for the same unchanged file/range MUST perform a real read instead of returning an unchanged-read stub that points to the pre-compact result
- **AND** file edit baseline validation for previously read full-file content remains available

#### Scenario: Auto compact resets in-turn guard state
- **WHEN** automatic compact succeeds during an active agent loop after the runtime guard has recorded pre-compact unchanged-read or low-signal attempts
- **THEN** the guard state from before compact MUST NOT block or synthesize cached results for post-compact tool calls

#### Scenario: Failed compact keeps guard state
- **WHEN** compaction is attempted but does not install a replacement AI-facing history
- **THEN** existing runtime read-observation and guard state MUST remain unchanged
