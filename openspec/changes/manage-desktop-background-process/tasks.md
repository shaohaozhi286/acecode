## 1. Managed Runtime Identity

- [x] 1.1 Add Desktop-managed protocol, runtime manifest, owner record, and generation-safe cleanup helpers
- [x] 1.2 Pass managed identity and Desktop owner information through daemon CLI and worker health
- [x] 1.3 Add runtime identity and cleanup unit coverage

## 2. Discovery and Supervision

- [x] 2.1 Extend daemon supervisors to attach, release, and stop both spawned and attached processes
- [x] 2.2 Change DaemonPool activation to connect-first, validate health compatibility, and replace verified incompatible generations
- [x] 2.3 Add explicit spawned/attached snapshots and policy-aware pool shutdown
- [x] 2.4 Cover reuse, replacement safety, and keep-alive shutdown in DaemonPool tests

## 3. Desktop Preference and UI

- [x] 3.1 Add the global disabled-by-default Desktop background continuation config field and serialization tests
- [x] 3.2 Add native get/set bridges that persist the preference and update the live pool policy
- [x] 3.3 Add the Desktop-only Settings switch using the established settings card style and exact approved copy
- [x] 3.4 Add web helper and presentation tests for bridge availability, state changes, and browser-only hiding

## 4. macOS Window Lifecycle

- [x] 4.1 Make macOS close requests hide the window without changing to accessory-only activation
- [x] 4.2 Restore the existing window from Dock activation and the menu-bar open action while preserving explicit quit
- [x] 4.3 Add lifecycle policy tests and perform a macOS desktop build check

## 5. Detached Interaction Continuity

- [x] 5.1 Make daemon permission prompts wait indefinitely by default while retaining explicit timeout and abort behavior
- [x] 5.2 Snapshot and replay unresolved AskUserQuestion requests on later WebSocket subscriptions
- [x] 5.3 Add tests for indefinite permission wait, question snapshots, replay shape, and first-response-wins behavior

## 6. Verification and Documentation

- [x] 6.1 Update daemon API/lifecycle documentation for managed health identity and background semantics
- [x] 6.2 Run focused C++ and web tests plus production web and desktop builds
- [x] 6.3 Validate the OpenSpec change and audit the final diff for unrelated or unsafe process handling changes

## 7. PID Reuse Recovery

- [x] 7.1 Specify proven PID reuse cleanup separately from ambiguous process identity
- [x] 7.2 Add tri-state daemon executable inspection and process-start-time generation checks
- [x] 7.3 Generation-safely discard proven stale Desktop runtime state without signaling the reused PID
- [x] 7.4 Add PID reuse, same-name process generation, and ambiguous-identity regression coverage
- [x] 7.5 Run focused tests, strict OpenSpec validation, Release builds, and a real restart smoke test
