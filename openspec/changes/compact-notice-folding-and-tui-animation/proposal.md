## Why

Successful compaction currently leaves several long system messages expanded in the transcript, which pushes useful conversation content off screen. The TUI also reuses the generic thinking indicator during compaction, so the operation has no distinct visual feedback.

## What Changes

- Mark the visible messages belonging to one compaction operation with stable lifecycle metadata.
- Keep compact progress and result details visible while the operation is incomplete, then automatically collapse the completed group into one `Context compacted` row in both TUI and Web.
- Keep the collapsed row user-expandable so the checkpoint, generated summary, and warning remain available.
- Add a TUI-only `Compacting conversation...` animation whose highlighted background is removed symmetrically from both edges toward the center and then repeats.
- Use the active dark/light theme for highlighted text and the terminal default background for the inward-moving edges.
- Leave failed compactions, ordinary system messages, tool folding, and `task_complete` presentation unchanged.

## Capabilities

### New Capabilities

- `compact-interaction-feedback`: Defines completed compact-notice grouping, live and replay folding behavior across TUI/Web, and the TUI compacting animation.

### Modified Capabilities

None.

## Impact

- Compaction transcript metadata and event payloads in `src/agent_loop.*`.
- TUI transcript replay/rendering, state, compact animation model, and keyboard expansion behavior.
- Web transcript projection and system-message rendering.
- Focused C++ and Web tests plus compact/session documentation.
