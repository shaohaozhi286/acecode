## ADDED Requirements

### Requirement: Exact Codex checkpoint contract
The system SHALL use the checkpoint-compaction prompt and summary prefix from the pinned Codex reference without ACECode-specific XML sections, response wrappers, or summary rewriting. The system SHALL append the checkpoint prompt as the final user message of the summary request and SHALL install the provider's final assistant text after the exact prefix and one newline as a user message.

#### Scenario: Successful local summary
- **WHEN** a local compaction request completes with assistant text
- **THEN** the replacement history ends with one user message containing the exact Codex summary prefix, one newline, and the assistant text

#### Scenario: Empty prior conversation
- **WHEN** manual compaction runs before any real user message exists
- **THEN** the provider still receives the checkpoint prompt and the returned prefixed summary becomes the replacement history

### Requirement: Codex replacement-history selection
The system SHALL build local replacement history from real user messages in the original pre-compact history and the newly prefixed summary. It SHALL exclude previous compact summaries, ACECode-generated user-role context, assistant messages, tool messages, tool calls, reasoning, and non-text content parts. It SHALL select user text from newest to oldest within a 20,000-token budget, partially truncate the oldest retained text when required, restore chronological order, and append the new summary last.

#### Scenario: User history fits the budget
- **WHEN** all eligible real user messages total at most 20,000 tokens
- **THEN** they are retained in chronological order before the new summary

#### Scenario: User history exceeds the budget
- **WHEN** eligible real user messages exceed 20,000 tokens
- **THEN** the newest messages are retained and only the oldest retained boundary message is partially truncated to fill the remaining budget

#### Scenario: Prior compact summary is present
- **WHEN** the original history contains a user message beginning with the exact compact summary prefix
- **THEN** that message is not selected as a real user message for the new replacement history

#### Scenario: Internal user-role context is present
- **WHEN** history contains goal, plan, todo, hook-continuation, transcript-only, or rebuilt session-context rows represented with the user role
- **THEN** those generated rows are not selected as real user messages for replacement history

### Requirement: Minimal context-overflow retry
The system SHALL handle a context-window-exceeded error from the summary request by removing exactly one oldest logical pre-prompt history item and retrying while always preserving the final checkpoint prompt. If that item is a tool call or tool output, the system SHALL also remove its matching counterpart so the retried provider request remains valid. It SHALL not remove a percentage of grouped turns. A terminal non-context error SHALL fail without installing replacement history.

#### Scenario: One item must be removed
- **WHEN** the first compaction request exceeds the provider context window and the second fits after removing its oldest history item
- **THEN** the system issues exactly two requests and the second request differs only by that one removed oldest item

#### Scenario: Oldest item has a tool counterpart
- **WHEN** overflow retry removes an oldest assistant tool call or tool output
- **THEN** the matching output or call is removed in the same retry and no orphan tool item is sent

#### Scenario: No removable item remains
- **WHEN** a compaction request containing only the checkpoint prompt still exceeds the context window
- **THEN** compaction fails and the original model history remains unchanged

#### Scenario: Terminal provider error
- **WHEN** the summary request fails with a non-context terminal provider error
- **THEN** the system reports the failure without installing a checkpoint or mutating compact-sensitive runtime state

### Requirement: Codex-compatible automatic thresholds
The system SHALL derive the effective input context window as 95 percent of the model's advertised context window and SHALL trigger automatic compaction at 90 percent of the advertised window, capped by the effective window. It SHALL compare the trigger against the greater of the latest server-reported total active-context token count and a fresh estimate of the current request history. Message count alone SHALL NOT trigger compaction.

#### Scenario: Server total reaches automatic threshold
- **WHEN** the provider reports total active-context usage at or above 90 percent of the advertised window
- **THEN** automatic compaction runs before the next model request

#### Scenario: Current history outgrows stale server usage
- **WHEN** newly appended user or tool content makes the fresh history estimate reach the threshold while the previous server total remains below it
- **THEN** automatic compaction runs before the next model request

#### Scenario: Many small messages remain below threshold
- **WHEN** history contains more than 256 messages but active-context usage remains below the token threshold
- **THEN** message count does not cause compaction

### Requirement: Pre-turn and mid-turn lifecycle
The system SHALL evaluate automatic compaction before every internal model sampling iteration. This SHALL cover the initial request and later follow-up requests after tool results. Pre-compact and post-compact hooks SHALL each run once around an actual attempt. The system SHALL NOT run micro-compaction, a repeated-failure circuit breaker, or summary-free history rescue.

#### Scenario: Initial request crosses threshold
- **WHEN** the active context crosses the automatic threshold before the first sampling iteration
- **THEN** compaction completes before the provider receives the normal model request

#### Scenario: Tool output crosses threshold
- **WHEN** a tool result appended during the turn causes the next request to cross the threshold
- **THEN** compaction completes before the follow-up model request

#### Scenario: Automatic compaction fails
- **WHEN** automatic compaction fails
- **THEN** the current sampling path ends with the error and no unsummarized history is discarded

### Requirement: Stable context without one-shot consumption
The system SHALL provide stable base/system instructions to the compaction summary request without draining or persisting one-shot hook/request context. Normal provider request construction SHALL continue to inject current session/request context after compacted history is installed.

#### Scenario: Pending hook request context
- **WHEN** one-shot hook context is pending while compaction runs
- **THEN** building the summary request does not consume it and the next normal model request still receives it

### Requirement: Append-only checkpoint persistence
The system SHALL append a hidden compact checkpoint containing the complete replacement model history while preserving the entire user-visible transcript. A successful compact SHALL replace only active model history, append a visible compact marker/summary, reset compact-sensitive read guards, and recompute active token state from the installed history. A failed compact SHALL perform none of those mutations.

#### Scenario: Successful checkpoint
- **WHEN** compaction succeeds
- **THEN** the JSONL session gains a compact checkpoint, the transcript retains all earlier turns, and active model history equals the checkpoint replacement history

#### Scenario: Failed checkpoint
- **WHEN** compaction fails before history installation
- **THEN** no checkpoint or transcript marker is appended and active history, read guards, and compact generation remain unchanged

### Requirement: Compact-window generations
Each successful compact SHALL advance a monotonically increasing compact-window number and record opaque first, previous, and current window identifiers in the checkpoint. Checkpoint decoding SHALL remain compatible with legacy checkpoints without window metadata.

#### Scenario: Consecutive compactions
- **WHEN** two compactions succeed in one session
- **THEN** the second checkpoint has the next window number, preserves the same first identifier, references the first checkpoint's current identifier as previous, and has a new current identifier

#### Scenario: Legacy checkpoint resume
- **WHEN** a session resumes from a checkpoint without compact-window fields
- **THEN** the checkpoint remains usable and the next successful compact initializes valid window metadata without losing replayed suffix messages

### Requirement: Resume and fork reconstruction
Resume and fork SHALL restore active model history from the newest valid compact checkpoint and replay only subsequent model-history entries in order, while retaining the append-only transcript and honoring existing rollback boundaries. A fork SHALL preserve the inherited checkpoint's replacement history while resetting its compact-window chain to a fresh UUIDv7 window zero; later resumes SHALL restore newer checkpoints created inside the fork.

#### Scenario: Resume after compact and suffix turns
- **WHEN** a session contains a compact checkpoint followed by additional turns
- **THEN** resumed model history consists of the checkpoint replacement history followed by those additional turns exactly once

#### Scenario: Fork after compact
- **WHEN** a fork point occurs after a compact checkpoint
- **THEN** the fork reconstructs the same compacted prefix and only the source suffix up to the fork point, but its next compact advances from a fresh window zero rather than the source window chain

#### Scenario: Resume a fork after its own compact
- **WHEN** a fork has created a new compact checkpoint and is later resumed
- **THEN** the fork restores its own latest compact-window number and identifiers instead of resetting again or inheriting the source chain

### Requirement: Provider-native compaction capability boundary
Providers that expose only Chat Completions messages SHALL use local Codex-compatible compaction. Native remote compaction SHALL be used only by a provider that explicitly supports native compaction triggers and validated native compaction response items; otherwise the system SHALL fall back to local compaction.

#### Scenario: Existing chat provider
- **WHEN** compaction runs through a provider without native compaction-item support
- **THEN** the local checkpoint prompt and replacement-history algorithm are used

#### Scenario: Incomplete native response
- **WHEN** a provider advertises native compaction but does not return one valid native compaction item
- **THEN** the response is not installed as native compacted history and the system uses or reports the defined local fallback

### Requirement: Compaction warning remains transcript-only
After a successful compaction, the system SHALL present a concise warning that long sessions and repeated compactions can reduce answer quality. This warning SHALL be recorded for the human transcript and SHALL NOT become an additional model-history message after the prefixed summary.

#### Scenario: Successful visible notification
- **WHEN** compaction succeeds
- **THEN** the user-visible transcript contains the compaction marker, summary, and quality warning while the model replacement history still ends with the prefixed summary user message
