## ADDED Requirements

### Requirement: Cached unchanged reads are scoped to current AI-facing history
The system SHALL only return an unchanged-read stub for a repeated `file_read` when the previous file/range result is still valid evidence for the current AI-facing history. A compact operation that replaces AI-facing history MUST invalidate unchanged-read observations created before that compact.

#### Scenario: Repeated read before compact uses unchanged stub
- **WHEN** the model calls `file_read` twice for the same file/range without an intervening compact and the file mtime has not changed
- **THEN** the second call MAY return an unchanged-read stub that references the previous result or persisted output path

#### Scenario: Repeated read after compact returns content
- **WHEN** the model calls `file_read` for a file/range, compact succeeds, and then the model calls `file_read` for the same unchanged file/range again
- **THEN** the post-compact call MUST return current file content rather than an unchanged-read stub or cached-read guard based only on pre-compact evidence
