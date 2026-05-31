## ADDED Requirements

### Requirement: Host-managed direct CDP backend

`ace-browser-host` SHALL provide a direct-CDP backend that can launch or connect to Chrome without requiring the browser extension to connect first. The backend SHALL own the browser-level CDP WebSocket and SHALL manage page targets and target session ids inside the host daemon.

The default launched Chrome profile SHALL be host-managed and persistent so authenticated internal sites can remain logged in across daemon restarts. Operators MAY override that profile directory with an environment variable.

#### Scenario: Ensure ready without extension

- **WHEN** the daemon is running and Chrome direct CDP is reachable
- **AND** no ace-browser-bridge extension has called `/plugin/hello`
- **THEN** `ensure-ready --json` SHALL return `ok: true`
- **AND** `data.ready` SHALL be `true`
- **AND** `data.backend` SHALL be `direct_cdp`
- **AND** `data.extension_connected` SHALL remain `false`

#### Scenario: Dynamic DevTools port

- **WHEN** the host launches Chrome for direct CDP
- **THEN** it SHALL use an ephemeral remote debugging port
- **AND** it SHALL discover the actual port and browser WebSocket path from `DevToolsActivePort`
- **AND** it SHALL report the discovered debug port in status diagnostics

#### Scenario: Persistent managed profile

- **WHEN** the host launches Chrome without an explicit profile override
- **THEN** it SHALL use a stable ACECode-managed user data directory
- **AND** a later `ensure-ready` SHALL be able to reuse the same profile's login state

### Requirement: Direct CDP readiness status

`status --json` SHALL expose separate direct-CDP and extension readiness fields. Overall `ready` SHALL be true when either direct-CDP is ready or the compatible extension backend is fresh.

#### Scenario: Direct backend is preferred

- **WHEN** direct-CDP is ready
- **AND** the extension backend is also fresh
- **THEN** `status --json` SHALL report `backend = direct_cdp`
- **AND** direct-CDP capabilities SHALL be enabled in `capabilities`

#### Scenario: Extension fallback remains available

- **WHEN** direct-CDP is not available
- **AND** the extension backend is fresh and protocol-compatible
- **THEN** `status --json` SHALL report `ready = true`
- **AND** `backend = extension`

### Requirement: Raw CDP uses host-managed connection

The `cdp --json --method <method>` command SHALL execute through the host-managed direct-CDP backend when it is ready. It SHALL NOT require `chrome.debugger` attachment by the extension.

#### Scenario: Debugger attachment conflict does not block raw CDP

- **WHEN** the extension backend reports a `cdp_unavailable` error caused by another debugger attached to a tab
- **AND** direct-CDP is ready
- **THEN** `cdp --json --method Browser.getVersion` SHALL still succeed through the direct backend

### Requirement: Direct command routing with fallback

For browser actions covered by the direct backend, `ace-browser-host` SHALL execute them directly through CDP. For actions not yet covered by the direct backend, the host MAY route to the extension backend when it is fresh and compatible. If neither backend can execute an action, the host SHALL return `backend_unavailable` with diagnostics for both backends.

#### Scenario: Navigate uses direct backend

- **WHEN** direct-CDP is ready
- **AND** the caller runs `navigate --json --operation goto --url https://example.com`
- **THEN** the command SHALL execute through direct CDP
- **AND** the result SHALL include the current URL from the page session

#### Scenario: Unsupported action falls back

- **WHEN** direct-CDP is ready but an action is not implemented by direct routing
- **AND** the extension backend is fresh
- **THEN** the host MAY enqueue the command for the extension backend
- **AND** the result SHALL identify the backend that executed it
