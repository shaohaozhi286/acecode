## Context

ACECode has two runtime mechanisms that reduce repeated reads:

- `MtimeTracker` stores read observations so a repeated `file_read` for the same unchanged file/range can return a compact "File unchanged since last read" stub.
- `AgentLoopDoomGuard` records low-signal tool results during an agent turn and can synthesize a cached-read guard after the model repeats an unchanged read.

Those mechanisms assume the earlier read result remains available in the model-visible conversation. Compact changed that assumption: the durable human transcript remains append-only, but the AI-facing `messages_` is replaced with a compacted `replacement_history`. If the replacement omits the earlier full `file_read` result, an unchanged-read stub no longer has evidence the model can use.

## Goals / Non-Goals

**Goals:**

- Make every successful compact boundary invalidate cached read observations that depend on pre-compact model-visible evidence.
- Reset in-turn doom-guard attempts when automatic, micro, or rescue compact succeeds inside an active agent loop.
- Keep file edit safety baselines intact, including prior full-file reads used to validate later edits.
- Keep the existing cached-read optimization for repeated reads within one un-compacted model-history epoch.

**Non-Goals:**

- Do not remove unchanged-read caching entirely.
- Do not change compact checkpoint storage, visible transcript behavior, or provider payload shape.
- Do not weaken edit safety checks or external modification detection.
- Do not introduce a new user-visible configuration flag.

## Decisions

### Decision 1: Separate read observations from edit baselines

Add an API on `MtimeTracker` to clear only read observations. This invalidates the "previous result is still current" cache without touching `records_`, which tracks file baselines for edit safety.

Alternative considered: clear the whole tracker after compact. That would unblock reads, but it would also erase valuable file-edit preconditions and could force unnecessary "read before edit" failures.

### Decision 2: Treat compact success as a model-history epoch boundary

Call the read-observation reset whenever `AgentLoop::apply_compact_result(...)` installs a compacted replacement history. This covers manual compact, auto full compact, auto micro-compact, and rescue compact because they converge on the same install path.

Alternative considered: reset only in the slash-command and auto-compact call sites. That duplicates lifecycle logic and risks missing future compact triggers.

### Decision 3: Reset local doom guard by generation

Because `AgentLoopDoomGuard` is local to `run_loop()`, manual compact naturally starts later turns with a new guard. Auto, micro, and rescue compact can occur inside the same `run_loop()` after the guard already recorded pre-compact results. A compact generation counter on `AgentLoop` lets the loop detect successful compaction and call `doom_guard.reset()` at the next safe point.

Alternative considered: pass the guard directly into compaction helpers. That couples compact lifecycle code to a per-turn guard implementation and makes manual compact awkward.

## Risks / Trade-offs

- [Risk] Clearing read observations after compact may cause one extra real `file_read` call for unchanged files. -> Mitigation: this is a small token/runtime cost and restores correctness when compact removed the previous evidence.
- [Risk] A successful compact that preserves the exact prior `file_read` result in `replacement_history` could have reused the cache. -> Mitigation: detecting that precisely would require scanning replacement history for matching tool-call content; the conservative reset is simpler and safe.
- [Risk] Future cache-like mechanisms may also depend on model-history visibility. -> Mitigation: keep the reset point at `apply_compact_result(...)` so additional compact-sensitive caches can hook into the same boundary.
