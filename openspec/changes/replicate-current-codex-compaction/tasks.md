## 1. Codex Compaction Core

- [x] 1.1 Replace the custom XML compact prompt and wrapper with the exact pinned Codex checkpoint prompt and summary prefix
- [x] 1.2 Implement real-user-message selection, prior-summary filtering, model-aware 20,000-token retention, and oldest-boundary truncation
- [x] 1.3 Replace grouped prompt trimming with oldest-item-at-a-time context-overflow retries that preserve the checkpoint prompt
- [x] 1.4 Add compact-core tests for empty history, exact request/summary shape, retention boundaries, prior summaries, and terminal failures
- [x] 1.5 Carry structured errors through non-streaming providers and add bounded abort-aware retries with strict context-overflow classification

## 2. Runtime Lifecycle and Accounting

- [x] 2.1 Replace fixed reserves and message-count triggering with 90-percent auto and 95-percent effective-window accounting using current-history and server-total usage
- [x] 2.2 Run the same non-destructive compact lifecycle before initial and tool-follow-up sampling without consuming one-shot request context
- [x] 2.3 Remove micro-compaction, failure circuit breaking, grouped trimming, and summary-free provider-overflow rescue from the active runtime
- [x] 2.4 Update agent-loop tests for pre-turn, mid-turn, hook ordering, stale server usage, many small messages, success resets, and failure atomicity
- [x] 2.5 Estimate pending pre-turn input without compacting it and inject mutable context at the Codex handoff boundary
- [x] 2.6 Add exact-order agent-loop regressions for pre-turn pending input and mid-turn summary finality

## 3. Persistence and Reconstruction

- [x] 3.1 Extend compact checkpoints with backward-compatible window number and first/previous/current identifiers
- [x] 3.2 Advance compact-window and token state atomically with successful history installation while preserving the append-only transcript and warning
- [x] 3.3 Update checkpoint, resume, registry, rollback, and fork tests for legacy metadata and compacted-prefix plus suffix replay

## 4. Documentation and Verification

- [x] 4.1 Update compact command, hook, session, and daemon documentation to describe the Codex-compatible behavior and removed legacy paths
- [x] 4.2 Run strict OpenSpec validation, focused compaction/session tests, the full unit suite, code-quality checks, and release build gates
- [x] 4.3 Document and validate handoff-boundary and transient-retry parity, then rebuild the runnable test instance
