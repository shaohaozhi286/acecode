## ADDED Requirements

### Requirement: macOS Desktop uses native local notifications
On macOS, `ACECode.app` SHALL use the UserNotifications framework to publish local notifications for configured permission, question, and normal-completion events. Initialization, authorization, or delivery failure MUST remain non-fatal.

#### Scenario: Supported macOS application
- **WHEN** the macOS Desktop shell starts with a valid application bundle
- **THEN** it initializes the native notification center independently of the tray icon

#### Scenario: Native notification is unavailable
- **WHEN** initialization fails, authorization is denied, or delivery is rejected
- **THEN** ACECode continues running and exposes or records the unavailable state without terminating the session

#### Scenario: Configured event is delivered
- **WHEN** a live permission, question, or normally completed turn passes the existing notification configuration and focus filters
- **THEN** ACECode submits a native notification with the event title, body, workspace, and session identity

### Requirement: macOS notification authorization is explicit and recoverable
The macOS Desktop shell SHALL expose the current operating-system notification authorization state. Enabling notifications while authorization is undetermined MUST request authorization, and denied authorization MUST be distinguishable from the ACECode configuration switch.

#### Scenario: User grants authorization
- **WHEN** the user enables notifications and grants the macOS authorization request
- **THEN** Settings reports the authorized state and subsequent eligible events can be delivered

#### Scenario: User denies authorization
- **WHEN** the user denies the macOS authorization request
- **THEN** Settings reports that system authorization is denied and offers recovery guidance without disabling unrelated ACECode functionality

#### Scenario: First eligible event precedes a decision
- **WHEN** an eligible notification is requested before authorization has been determined
- **THEN** ACECode requests authorization once and delivers the retained event if authorization is granted

### Requirement: notification activation restores the exact session
Every macOS notification SHALL retain its own workspace and session identity. Selecting a delivered notification while ACECode is running MUST foreground the Desktop window and open the session encoded by that notification.

#### Scenario: Current-workspace notification is selected
- **WHEN** the user selects a notification for a session in the active workspace
- **THEN** ACECode restores the window and selects that session through the existing Desktop focus path

#### Scenario: Cross-workspace notification is selected
- **WHEN** the user selects a notification for a registered workspace that is not active
- **THEN** ACECode restores the window, activates the target workspace, and opens that session

#### Scenario: Older notification is selected
- **WHEN** several notifications have been delivered and the user selects an older one
- **THEN** ACECode opens the older notification's session rather than the newest notification's session

### Requirement: notification lifecycle is safe
The macOS notification implementation MUST ignore stale asynchronous callbacks after shutdown and MUST clear application callbacks before objects captured by those callbacks are destroyed.

#### Scenario: Authorization completes during shutdown
- **WHEN** an authorization or settings callback completes after notification shutdown begins
- **THEN** the stale callback performs no UI or session mutation

#### Scenario: Application exits with delivered notifications
- **WHEN** `ACECode.app` shuts down normally
- **THEN** it detaches the notification delegate and clears pending and delivered notifications so stale alerts cannot activate destroyed runtime state

### Requirement: existing platforms remain compatible
The portable notification contract SHALL preserve Windows WinToast behavior and safe no-op behavior on unsupported platforms. This change MUST NOT add macOS standalone TUI notifications.

#### Scenario: Windows notification is delivered
- **WHEN** a Windows TUI or Desktop caller uses the portable notification facade
- **THEN** the existing WinToast delivery and activation behavior remains available

#### Scenario: Unsupported platform uses the facade
- **WHEN** a platform without a native backend initializes or publishes a notification
- **THEN** the call fails safely without platform-specific dependencies or a process failure

#### Scenario: macOS standalone TUI runs
- **WHEN** the standalone macOS TUI starts
- **THEN** it does not request `ACECode.app` notification authorization or claim Desktop notification identity
