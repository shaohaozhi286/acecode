## 1. Runtime Reset Primitives

- [x] 1.1 Add a `MtimeTracker` API that clears read observations without clearing edit baselines.
- [x] 1.2 Add an `AgentLoopDoomGuard` reset API that clears attempts and cooldowns.

## 2. Compact Integration

- [x] 2.1 Invalidate read observations whenever `AgentLoop::apply_compact_result()` installs replacement history.
- [x] 2.2 Add compact-generation tracking so active agent loops reset their local doom guard after auto, micro, or rescue compact succeeds.

## 3. Regression Coverage

- [x] 3.1 Add focused unit coverage proving read observations clear independently from edit baselines.
- [x] 3.2 Add focused doom-guard coverage proving reset clears cached-read/low-signal guard state.
- [x] 3.3 Add a file-read regression proving a compact-style reset lets the same unchanged file/range return real content again.

## 4. Verification

- [x] 4.1 Run focused C++ tests for file-read cache, doom guard, and tracker behavior.
- [x] 4.2 Run `openspec validate reset-read-guards-after-compact --strict`.
- [x] 4.3 Run repository-level consistency checks needed before committing.
