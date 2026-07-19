## ADDED Requirements

### Requirement: Permission requests wait without a connected UI
Daemon permission requests SHALL remain pending without a passive default timeout when no Desktop or channel client is connected.

#### Scenario: Desktop disconnects during permission request
- **WHEN** a permission request is unresolved and all WebSocket clients disconnect
- **THEN** the request remains pending until a valid response, abort, shutdown, or permission-mode transition resolves it

#### Scenario: Multiple surfaces answer
- **WHEN** Desktop and another channel submit decisions for the same request
- **THEN** the first valid decision wins and later decisions do not change the result

### Requirement: Pending interactions are replayed to later subscribers
After a session subscription is acknowledged, the daemon SHALL replay every unresolved permission and AskUserQuestion request for that session to the new connection.

#### Scenario: Desktop reconnects to a pending permission
- **WHEN** Desktop subscribes after the permission request event was originally emitted
- **THEN** it receives a reconstructed permission request with the original request ID, tool, and arguments

#### Scenario: Desktop reconnects to a pending question
- **WHEN** Desktop subscribes after the question request event was originally emitted
- **THEN** it receives a reconstructed question request with the original request ID and questions

#### Scenario: Replayed and live event overlap
- **WHEN** a request is both observed live and included in a pending snapshot
- **THEN** the stable request ID allows the client to de-duplicate it and only one answer is accepted

### Requirement: Explicit question policies retain their meaning
The interaction-continuity behavior MUST NOT remove an explicitly configured AskUserQuestion deny or timeout policy.

#### Scenario: Explicit timeout policy
- **WHEN** a session intentionally configures an AskUserQuestion timeout
- **THEN** the question follows that configured timeout even when no UI is connected
