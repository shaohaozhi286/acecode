## Why

ACECode currently compacts with a custom keep-last-turns summarizer, a separate micro-compact pass, fixed token reserves, and a lossy rescue path. Those behaviors diverge from the current Codex implementation in `N:\Users\shao\codex` and cause early compaction, stale-token decisions, incomplete summaries, and post-compact history shapes that the model was not trained to consume.

## What Changes

- **BREAKING** Replace the custom `keep_turns` compaction algorithm with Codex's current local compaction algorithm: summarize the complete effective model history, retain recent real user messages within a 20,000-token budget, and append the prefixed handoff summary as the final user message.
- Use Codex's exact checkpoint prompt and summary prefix instead of ACECode's nine-section XML summary prompt and wrapper text.
- Align automatic compaction with Codex's context accounting: use server-reported total active-context usage when available, fall back to a current history estimate, derive the automatic threshold at 90% of the model context window, and treat 95% as the hard effective input window.
- Run the same compaction lifecycle before a model request and between tool-follow-up model requests; remove the independent message-count trigger, micro-compact-first pass, repeated-failure circuit breaker, and summary-free rescue truncation.
- When a compaction request itself exceeds context, retry by removing the oldest model-history item one at a time while always retaining the compaction prompt; remove a matching tool call/output counterpart with that item when required to keep the request valid, matching Codex's local fallback behavior.
- Preserve the append-only human transcript while storing the Codex-shaped replacement model history in compact checkpoints for resume and fork.
- Track compact-window generations and token state so each successful compact starts a fresh model-history epoch and subsequent usage is recomputed from the installed replacement history.
- Keep provider-native remote compaction capability-gated. ACECode's current providers expose chat-message turns rather than Responses API items, so unsupported providers use the Codex local algorithm instead of fabricating an incompatible remote-compaction payload.

## Capabilities

### New Capabilities

- `codex-compaction-parity`: Defines Codex-compatible compaction prompting, replacement-history construction, automatic trigger accounting, overflow retry, checkpoint persistence, and resume/fork behavior.

### Modified Capabilities

None.

## Impact

- Core implementation: `src/commands/compact.*`, `src/commands/compact_prompt.*`, `src/agent_loop.*`, and compact-sensitive runtime guards.
- Session lifecycle: `src/session/compact_checkpoint.*`, resume/registry/fork reconstruction, and compact checkpoint metadata.
- Tests: compact core, agent-loop compaction lifecycle, checkpoint round trips, resume, registry, and fork coverage.
- Documentation: user manual, daemon API, and hook descriptions that currently describe the old replacement shape or micro/rescue behavior.
- No generated frontend output or vendored/submodule source is edited.
