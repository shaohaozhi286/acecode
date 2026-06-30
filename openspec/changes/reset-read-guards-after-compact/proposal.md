## Why

Repeated `file_read` calls can be skipped by the unchanged-read cache and later by the doom guard because the previous read result is assumed to still be available in model history. After compact, that assumption can be false: the human transcript still contains the old read, but the AI-facing `replacement_history` may not, leaving the model with only a guard response and no recoverable content.

## What Changes

- Reset compact-sensitive read-observation state after successful manual, automatic, micro, or rescue compact installs a new AI-facing history.
- Reset in-turn doom-guard state when compact succeeds during an active agent loop, so unchanged-read guard decisions from pre-compact history cannot block post-compact reads.
- Preserve edit safety baselines and external-modification tracking; only the cache/guard state that depends on old model-visible evidence is reset.
- Add regression coverage proving a compact boundary allows the same unchanged `file_read` to produce real content again.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `context-compaction`: Successful compaction invalidates runtime guard/cache state that depends on the pre-compact AI-facing history.
- `tool-result-storage`: Cached unchanged `file_read` results may only be reused while their referenced prior result remains valid for the current AI-facing history.

## Impact

- Affected C++ areas: `src/agent_loop.*`, `src/agent_loop_doom_guard.*`, `src/tool/mtime_tracker.*`, and `src/tool/file_read_tool.cpp` behavior through existing tracker APIs.
- Affected tests: focused unit tests for `MtimeTracker`, `AgentLoopDoomGuard`, `file_read`, and an agent-loop compact regression where practical.
- No protocol, provider API, or dependency changes are expected.
