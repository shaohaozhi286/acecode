## ADDED Requirements

### Requirement: Windows notifications use WinToast
On Windows, ACECode SHALL use the WinToast library for native session notifications in both the TUI and Desktop executables. Native notification initialization or delivery failure MUST remain non-fatal, and non-Windows builds MUST retain no-op behavior.

#### Scenario: Supported Windows host
- **WHEN** either ACECode Windows executable starts on a WinToast-compatible system
- **THEN** it initializes a surface-specific WinToast application identity and can publish native notifications without depending on the system-tray icon

#### Scenario: Notification subsystem unavailable
- **WHEN** WinToast is unsupported or initialization or delivery fails
- **THEN** ACECode continues running and records a diagnostic instead of terminating the session

#### Scenario: Non-Windows build
- **WHEN** ACECode is built or run on a non-Windows platform
- **THEN** the notification interface remains available as a no-op without requiring WinToast

### Requirement: Completed turns notify from TUI and Desktop
ACECode SHALL publish a completion notification from both the Windows TUI and Desktop surfaces when an active assistant turn finishes normally with user-visible assistant output. It MUST NOT publish a completion notification for replayed history, a user-aborted turn, or an error termination.

#### Scenario: Desktop turn completes
- **WHEN** a live Desktop session transitions from busy to complete with final assistant text and notification configuration permits completion alerts
- **THEN** ACECode publishes a WinToast notification identifying the completed workspace and session

#### Scenario: TUI turn completes
- **WHEN** a live TUI session transitions from busy to complete with final assistant text and notification configuration permits completion alerts
- **THEN** ACECode publishes a WinToast notification identifying the current session

#### Scenario: Turn is aborted or fails
- **WHEN** a TUI or Desktop turn is aborted by the user or terminates as an error
- **THEN** ACECode does not publish a task-completion notification for that turn

#### Scenario: Focus suppression is enabled
- **WHEN** the completed session is already visible in the foreground and `suppress_when_focused` is enabled
- **THEN** ACECode suppresses the redundant completion notification

### Requirement: Notification activation opens the completed session
Each native notification SHALL retain its own workspace and session identity. Clicking a completion notification MUST restore and foreground the originating ACECode window and select the session carried by that notification, even when newer notifications have subsequently been shown.

#### Scenario: Desktop notification for current workspace
- **WHEN** the user clicks a Desktop completion notification whose workspace is already active
- **THEN** the Desktop window is restored and foregrounded and the Web UI resumes and selects the notification's session

#### Scenario: Desktop notification for another workspace
- **WHEN** the user clicks a Desktop completion notification for a different registered workspace
- **THEN** the Desktop window is restored, the target workspace is activated, and the Web UI opens the notification's session

#### Scenario: TUI notification is clicked
- **WHEN** the user clicks a TUI completion notification while the TUI process is running
- **THEN** the terminal window is restored and foregrounded and the FTXUI thread resumes the notification's session by stable session id

#### Scenario: Several notifications are outstanding
- **WHEN** two sessions publish notifications before the user clicks the older notification
- **THEN** clicking the older notification still opens the session encoded by that older notification rather than the most recently published session

### Requirement: Existing Desktop notification behavior remains compatible
The Desktop bridge SHALL continue to support question notifications, existing notification configuration, and both object and JSON-string payload arguments while using the WinToast backend.

#### Scenario: Existing Web bridge sends a JSON string
- **WHEN** the Web UI invokes the native notification bridge with its existing JSON-string payload form
- **THEN** the native bridge decodes and delivers the same structured payload accepted in object form

#### Scenario: Question notification
- **WHEN** a live Desktop session requests user input and question notifications are enabled
- **THEN** ACECode publishes the question through WinToast and clicking it opens the requesting session
