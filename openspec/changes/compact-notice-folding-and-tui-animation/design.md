## Context

ACECode persists and broadcasts compaction progress, checkpoint, summary, and warning as ordinary transcript-only system messages. That preserves an append-only audit trail, but neither surface knows that the messages belong to one completed operation. The TUI therefore renders the generated summary at full height, while the Web treats each message as an unrelated generic system row. The TUI waiting row also has no compact-specific phase and currently renders the normal thinking shimmer.

The change must preserve the canonical compact history and append-only JSONL contract introduced by `replicate-current-codex-compaction`. It must also preserve the special always-expanded `task_complete` Markdown behavior and ordinary tool-result folding.

## Goals / Non-Goals

**Goals:**

- Give all visible notices from one compact operation a stable identity and explicit completion edge.
- Show incomplete details, then reduce a successful operation to one expandable row on both TUI and Web, including after resume/reload.
- Give the TUI an exact `Compacting conversation...` indicator with a repeated symmetric inward-background animation.
- Reuse existing theme colors, animation scheduling, and transcript persistence paths.

**Non-Goals:**

- Changing the compact prompt, token accounting, replacement history, checkpoint payload, or warning text.
- Folding failed compaction errors or unrelated system/tool/task-completion output.
- Adding the compact background animation to Web.
- Adding a separate animation thread or changing legacy/conhost cadence.

## Decisions

### Persist explicit compact-notice lifecycle metadata

Each visible compact notice will carry `compact_notice: true`, a UUIDv7 `compact_notice_id`, a stage name, and `compact_notice_complete`. Manual and automatic compaction generate one notice ID before the provider request and reuse it through the final warning. The final warning is the successful completion edge.

Metadata is preferable to matching English content because the renderer must remain correct if labels are localized or wording changes. It also gives live Web events and restored JSONL messages the same grouping input. Existing clients can ignore the additional object fields.

### Project one UI row without changing append-only persistence

The backend will continue persisting all original messages in their original order. While a group has no completion edge, its notices remain visible and expanded. Once complete:

- TUI live handling and session replay merge the notices into one runtime `compact_notice` row, default it to collapsed, and retain the joined source text for `Ctrl+E`/`Ctrl+O` expansion.
- Web transcript projection replaces completed notices with one synthetic system message labeled `Context compacted`; clicking expands the joined source text.

Failures never receive the successful completion edge, so the progress/error output remains visible. Old sessions without metadata retain their existing presentation.

### Drive compaction state from transcript metadata in TUI

`AgentCallbacks` will gain a metadata-preserving transcript-message callback. TUI uses the progress notice to set `is_compacting` and a monotonic animation origin, and the successful completion notice or final busy transition clears it. This avoids a second phase channel and keeps visible state synchronized with the same lifecycle used for folding.

### Use an elapsed-time symmetric mask for the TUI background

A pure compact-animation model will convert elapsed time plus glyph count into a highlighted-background mask. At the start of a cycle every glyph is highlighted. At successive phases, one glyph at each edge returns to the terminal default background until the fronts meet at the center; the next cycle resets to fully highlighted.

The renderer uses the exact glyph sequence `Compacting conversation...`, `selection_bg`/`selection_fg` for the highlighted center, and no background decorator plus theme-appropriate text for cleared edges. It reuses the existing active 20 ms ticker, so dropped redraws recover from elapsed time and no new thread is required.

## Risks / Trade-offs

- **A client receives only part of a notice group** -> It leaves those messages expanded because no completion edge is present.
- **The final event arrives before an intermediate repaint** -> The final state is still correctly collapsed and all text remains available on expansion; no arbitrary delay is introduced.
- **A compact failure follows a progress notice** -> Busy teardown clears TUI animation state, while the uncompleted notice and error remain visible.
- **Old sessions have untagged compact text** -> They are not guessed from content and keep the legacy presentation.
- **Per-glyph background changes can be stepped in terminals** -> The 20 ms elapsed-time scheduler keeps timing stable; the intended compression boundary remains cell-aligned by design.

## Migration Plan

1. Add metadata to new compact notice messages without changing their content or order.
2. Deploy TUI/Web grouping readers that tolerate missing fields.
3. Existing session files remain readable; no data migration is required.
4. Rollback is safe because older code ignores unknown metadata and still displays every persisted message.

## Open Questions

None.
