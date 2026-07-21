## Context

ACECode currently has four independent context-reduction mechanisms: a keep-last-four-turns summarizer, destructive micro-compaction of old tool observations, a three-failure circuit breaker, and a summary-free overflow rescue. They make decisions from different estimates and install different history shapes. The result is not equivalent to the current Codex behavior and makes resume/fork behavior difficult to reason about.

The parity reference is `N:\Users\shao\codex` at commit `da61f7d8e1` (2026-07-21). Its local compaction path uses one checkpoint prompt, retries a context-overflowing checkpoint request by removing one oldest history item at a time, and installs a replacement history made from recent real user messages plus a prefixed summary as the final user message. Its rollout remains append-only and records the replacement history as a compacted checkpoint.

ACECode providers currently expose Chat Completions-style `ChatMessage` histories. They do not expose Responses API `CompactionTrigger` or encrypted `Compaction` response items. ACECode also rebuilds its stable system/session context for every provider request, while one-shot hook/request context must not be consumed merely to prepare a compaction request.

## Goals / Non-Goals

**Goals:**

- Match Codex's current local compaction prompt, prefix, overflow retry, retained-user-message budget, replacement-history shape, automatic thresholds, and pre-/mid-turn lifecycle.
- Keep the user-visible transcript append-only while making model-history checkpoints authoritative for resume and fork.
- Make token accounting and compact-window generations explicit so a successful compact starts a fresh active-context epoch.
- Remove competing ACECode-only reduction and recovery paths that can silently discard unsummarized context.
- Preserve provider extensibility with an explicit native-compaction capability boundary.

**Non-Goals:**

- Migrating every ACECode provider from Chat Completions to the Responses API.
- Emulating remote Responses API compaction with ordinary chat messages.
- Reproducing Codex's Rust types or rollout JSON schema byte-for-byte.
- Rewriting the user-visible transcript into the model's compacted history.
- Editing generated frontend assets or vendored dependencies.

## Decisions

### 1. Pin parity to the current Codex local algorithm

The compactor SHALL append Codex's exact checkpoint prompt as a final user message to the effective pre-compact model history. The provider's last assistant text becomes the summary suffix without XML extraction or a second transformation. The installed summary SHALL be Codex's exact summary prefix, a newline, and that suffix, represented as a user message.

The alternative was to keep ACECode's nine-section XML prompt and merely adjust thresholds. That retains the incompatible response contract and replacement shape, so it does not achieve parity.

### 2. Retain recent real user messages within a 20,000-token budget

After a successful summary request, the compactor SHALL scan the original history for real user messages, excluding earlier compact summaries and ACECode-generated user-role context such as goal, plan, todo, hook-continuation, transcript-only, and rebuilt session-context rows. It SHALL select from newest to oldest up to 20,000 text tokens, partially truncate the oldest selected message when required, restore chronological order, and append the new summary as the last user message. Assistant messages, tool messages, reasoning, tool calls, and content-part images are not copied by the local replacement builder; their important state must be represented by the summary.

This deliberately replaces `keep_turns=4`. A fixed turn count is unrelated to context size and can preserve a very large tail or discard many small but important user constraints.

### 3. Retry checkpoint overflow one item at a time

If the compaction request receives a context-window-exceeded error, the compactor SHALL remove exactly the oldest pre-prompt history item and retry, preserving the checkpoint prompt. As Codex's `ContextManager::remove_first_item` does, removing a function/tool call or output also removes its corresponding counterpart so the next provider request is structurally valid; this paired cleanup is part of removing that one logical history item. It SHALL continue while at least one removable item remains. Ordinary retryable transport errors continue to use the provider's normal retry behavior; non-context terminal errors fail the compact operation without installing new history.

Dropping approximately 20 percent of grouped turns was rejected because it removes more context than required and does not match Codex.

### 4. Use Codex-compatible active-context thresholds

The effective input window SHALL be 95 percent of the model's advertised context window. Automatic compaction SHALL begin at 90 percent of the advertised window, capped by the effective window. The trigger SHALL use the greater of the latest server-reported total active-context usage and a fresh estimate of the current request history, so newly appended user/tool messages cannot be hidden by stale usage.

The independent 256-message trigger, fixed 13k/20k reserves, and micro-compact-first trigger are removed. They describe ACECode-specific policies rather than Codex's context-window policy.

### 5. Reuse the existing top-of-loop lifecycle for pre- and mid-turn compaction

ACECode already checks at the top of each internal sampling iteration. The first iteration provides pre-turn compaction; subsequent iterations after tool results provide mid-turn compaction. The pre-compact and post-compact hooks SHALL surround one actual compact attempt exactly once. A failed automatic compact SHALL leave the old model history intact and end the current sampling path with the error instead of invoking a summary-free rescue.

This keeps the lifecycle in one place and avoids creating separate pre-request and post-tool implementations.

### 6. Build compaction context without consuming one-shot request context

The summary request SHALL include stable base/system instructions and the existing model history, but SHALL not call helpers that drain hook-provided request context or mutate the pending user message. Normal request building continues to reinject current system/session context after replacement history is installed.

The replacement checkpoint stores only the Codex-shaped retained user messages and summary. It does not persist transient hook context, the compact prompt, or the assistant summary response as a normal assistant turn.

### 7. Preserve append-only persistence and add compact-window identity

On success, ACECode SHALL continue appending a hidden compact checkpoint to the JSONL session and a visible compact marker/summary to the human transcript. It SHALL replace only the in-memory model history. The checkpoint schema gains compact-window metadata: monotonically increasing window number plus first, previous, and current window identifiers. Decoding remains backward compatible with existing checkpoints that lack those fields.

Resume SHALL reconstruct from the newest valid compact checkpoint and replay only the suffix after it, preserving the existing rollback semantics. A fork preserves that effective compacted history but resets the latest inherited checkpoint's compact-window metadata to a fresh UUIDv7 window zero. A later resume of that fork restores any newer fork-local checkpoint normally. The user-visible transcript is never truncated.

Window ids use UUIDv7, matching Codex's time-ordered compaction window identifiers. Ordering remains explicit in `window_number`; consumers do not need to infer it from the UUID text.

### 8. Keep remote compaction capability-gated

All current Chat Completions providers use local compaction. A future provider may opt into native compaction only if it can send a native compaction trigger and return a native compaction item with a validated replacement history. Unsupported or incomplete implementations SHALL fall back to the local path.

This avoids pretending that an ordinary chat summary is Codex's Responses API remote-compaction protocol.

### 9. Remove legacy lossy reducers rather than leaving dormant policy

Micro-compaction, compact failure circuit-breaking, grouped 20-percent trimming, and summary-free rescue SHALL be removed from the active runtime and public compact API. Existing observation-read guards are still reset after a successful full compact because the installed model history starts a new context epoch; they remain unchanged after failure.

Leaving the reducers dormant was rejected because later code paths could reactivate incompatible behavior and their tests would continue asserting obsolete contracts.

### 10. Keep pending pre-turn input outside the compactor and preserve the handoff boundary

Before the first sampling iteration, ACECode SHALL estimate the request with the pending user input included, but SHALL run compaction against only the already-recorded model history. The pending input is recorded after a successful or failed pre-turn compact attempt and is never copied into that attempt's summary request. The first sampling iteration reuses that completed preflight decision; only later tool-follow-up iterations may compact the active turn.

For every normal provider request, rebuilt session, time/CWD, hook, plan-mode, and todo context SHALL remain separate messages. They SHALL be inserted immediately before the last real user message, or before the compact summary when no real user message remains. They SHALL never be merged into the summary text or appended after a mid-turn summary. This preserves the exact summary prefix and keeps the summary as the final model-history item after mid-turn compaction, while pre-turn compaction naturally leaves the newly recorded user input last.

Temporarily removing an already-persisted pending input during compaction was rejected because the checkpoint would then be appended after that input and resume could not replay the input as a post-checkpoint suffix without duplication.

### 11. Make compaction retries consume structured non-streaming errors

`ChatResponse` SHALL carry the same `ProviderErrorInfo` used by streaming errors. OpenAI-compatible and Anthropic non-streaming calls SHALL populate the error kind, HTTP status, response body, request id, and retryability instead of exposing only an `[Error]` string. The compactor SHALL retry structured retryable non-context errors up to `stream_max_retries`, using abort-aware exponential backoff. Removing an item for an explicit context overflow resets the transient retry counter, matching Codex's retry loop.

Context overflow classification SHALL require a structured HTTP error at a plausible status plus a recognized provider code or strong context-length phrase. A generic 413, generic mentions of token limits, or unrelated payload-size text SHALL NOT delete history. Legacy string-only responses remain terminal except for the narrow context-overflow fallback needed by third-party provider implementations.

## Risks / Trade-offs

- [A summary request may itself be expensive] -> Trigger from active-context usage, preserve the prompt, and trim only one oldest item per overflow retry as Codex does.
- [Chat providers tokenize differently] -> Prefer server totals when available and keep token estimation model-aware where ACECode supports it; use conservative text estimation only as fallback.
- [Removing micro-compaction can increase tool-output pressure before the threshold] -> The full compactor now runs between tool follow-ups using current request size, and overflow is never handled by silent unsummarized deletion.
- [A malformed or weak model summary loses assistant/tool detail] -> Use the exact Codex handoff contract and keep the full append-only transcript for human inspection and recovery.
- [Old checkpoints lack window metadata] -> Decode them as legacy window zero and initialize the next window deterministically at resume.
- [Provider error classification may vary] -> Centralize context-window-exceeded detection and cover representative provider errors with focused tests.
- [Backoff makes synchronous compaction take longer during outages] -> Bound it by the provider stream retry budget, honor cancellation between short sleep slices, and leave history/checkpoint state unchanged on exhaustion.
- [Moving pre-turn compaction before persistence changes transcript event ordering] -> Keep the user input visible through the normal accepted-input UI path, then persist it exactly once after the compact attempt so checkpoint replay order remains authoritative.

## Migration Plan

1. Add the Codex prompt/prefix constants and pure replacement-history/token-budget helpers with unit tests.
2. Replace the compact execution and overflow retry while retaining the existing provider abstraction.
3. Replace automatic threshold accounting and remove legacy micro/rescue/circuit-breaker paths.
4. Extend checkpoint metadata and update resume/fork round-trip tests with legacy fixtures.
5. Preserve the normal-request handoff boundary and add structured non-streaming retry behavior.
6. Update user and daemon/hook documentation, then run focused compaction/session tests, the full unit suite, code-quality checks, and build gates.

Rollback is a normal source revert: existing v1 checkpoints remain readable, and new metadata is optional to older readers that already ignore unknown JSON fields.

## Open Questions

None. Remote Responses API compaction remains explicitly deferred until a provider exposes the required native item protocol.
