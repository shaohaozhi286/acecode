## ADDED Requirements

### Requirement: Compact notices expose one durable lifecycle
The system SHALL attach a stable operation identifier, stage, and completion flag to every transcript-only notice emitted by one manual or automatic compaction without changing the notice text or append-only message order.

#### Scenario: Successful compaction notice sequence
- **WHEN** a manual or automatic compaction reaches its progress, checkpoint, summary, and warning notices
- **THEN** all notices SHALL share one non-empty operation identifier and only the terminal successful notice SHALL mark the group complete

#### Scenario: Compact failure remains incomplete
- **WHEN** compaction reports progress but fails before installing replacement history
- **THEN** the system SHALL NOT mark that compact-notice group successfully complete

### Requirement: Completed compact details fold consistently
The TUI and Web SHALL show incomplete compact-notice details and SHALL replace a successfully completed group with one collapsed `Context compacted` presentation while retaining all original details for expansion.

#### Scenario: Live compact completion
- **WHEN** the completion notice arrives after compact progress and generated summary details
- **THEN** each surface SHALL collapse the group into one row instead of leaving the long summary expanded in the transcript

#### Scenario: Resume or reload after compaction
- **WHEN** a TUI session is resumed or Web history is loaded from persisted tagged notices
- **THEN** the completed operation SHALL be reconstructed as the same single collapsed row

#### Scenario: User expands compact details
- **WHEN** the user expands the completed compact row with the surface's normal disclosure control
- **THEN** the progress, checkpoint, generated summary, and warning text SHALL be available in original order

#### Scenario: Incomplete or failed compaction
- **WHEN** no successful completion notice exists for a compact operation
- **THEN** its progress and error information SHALL remain visible rather than being hidden in a success-style collapsed row

### Requirement: TUI uses a distinct compacting animation
While compaction is active in the modern TUI, the waiting row SHALL display the exact text `Compacting conversation...` with a highlighted background that contracts symmetrically from both edges toward the center and repeats.

#### Scenario: Animation begins
- **WHEN** a compact progress notice starts a TUI compaction phase
- **THEN** the compact phrase SHALL replace the generic thinking phrase and begin from a fully highlighted background

#### Scenario: Background contracts inward
- **WHEN** animation time advances through one cycle
- **THEN** matching glyphs at the left and right edges SHALL return to the terminal default background until the fronts meet at the center

#### Scenario: Animation repeats deterministically
- **WHEN** the inward fronts reach the center or redraw frames are skipped
- **THEN** the next cycle SHALL restart from the elapsed-time-derived fully highlighted state without depending on prior rendered frames

#### Scenario: Dark and light themes
- **WHEN** the active TUI theme is dark or light
- **THEN** highlighted glyphs SHALL use the theme selection colors and cleared glyphs SHALL use the terminal default background with theme-readable text

### Requirement: Existing transcript presentation remains scoped
Compact notice folding SHALL NOT change ordinary system messages, tool-result folding, or the always-expanded Markdown presentation of `task_complete` summaries.

#### Scenario: Non-compact transcript rows
- **WHEN** a transcript row has no compact-notice metadata
- **THEN** its existing TUI and Web rendering and expansion behavior SHALL remain unchanged
