## ADDED Requirements

### Requirement: Desktop discovers a reusable managed process before spawning
ACECode Desktop SHALL inspect its reserved runtime state and attach to a healthy Desktop-managed process when the runtime identity, live health identity, and Desktop protocol are compatible.

#### Scenario: Compatible process already exists
- **WHEN** Desktop starts and a healthy compatible Desktop-managed process is recorded
- **THEN** Desktop attaches to that process and uses its existing port and token without spawning another process

#### Scenario: Current preference is disabled but a crash residue exists
- **WHEN** Desktop starts with background continuation disabled and finds a healthy compatible managed process left by an abnormal prior exit
- **THEN** Desktop attaches to it and applies the disabled preference to the next real app quit

#### Scenario: No reusable process exists
- **WHEN** Desktop starts and no healthy verified compatible managed process exists
- **THEN** Desktop spawns one managed process on a newly selected loopback port

### Requirement: Incompatible managed processes are replaced safely
Desktop MUST stop and replace an incompatible process only after verifying that it is Desktop-managed, and MUST NOT prompt the user before a verified compatibility replacement.

#### Scenario: Verified protocol mismatch
- **WHEN** a live process in the reserved Desktop runtime declares an incompatible Desktop protocol
- **THEN** Desktop stops its managed process group, waits for termination, and starts a compatible replacement without a user prompt

#### Scenario: Process identity is ambiguous
- **WHEN** runtime files do not prove that the recorded PID is the expected Desktop-managed ACECode process
- **THEN** Desktop does not signal that PID and reports activation failure

#### Scenario: Standalone CLI daemon exists
- **WHEN** a standalone CLI daemon is running in its normal runtime location
- **THEN** Desktop neither connects to it nor stops or garbage-collects it

### Requirement: Managed process source and exit policy are explicit
Desktop SHALL track whether its active process was spawned or attached and SHALL apply a separate stop-with-Desktop or keep-alive policy at real application shutdown.

#### Scenario: Disabled preference on real quit
- **WHEN** “退出 ACECode 后继续运行后台进程” is disabled and the user really quits ACECode
- **THEN** Desktop stops the active managed process whether it was spawned or attached

#### Scenario: Enabled preference on real quit
- **WHEN** “退出 ACECode 后继续运行后台进程” is enabled and the user really quits ACECode
- **THEN** Desktop releases local supervision resources without terminating the active managed process

#### Scenario: Preference changes from enabled to disabled
- **WHEN** the user disables background continuation while a process is running
- **THEN** the process continues running immediately and is stopped on the next real app quit

#### Scenario: Preference changes from disabled to enabled
- **WHEN** the user enables background continuation while a process is running
- **THEN** the active supervisor is updated so Desktop termination does not kill the process

### Requirement: Persistence preference is global, Desktop-only, and disabled by default
The system SHALL store a global Desktop preference with default `false` and SHALL expose it only when the native Desktop bridge is available.

#### Scenario: Fresh configuration
- **WHEN** no persistence preference exists in the configuration
- **THEN** the Settings switch is off and a real Desktop quit stops the managed process

#### Scenario: Desktop Settings renders the control
- **WHEN** the General settings section is opened inside ACECode Desktop
- **THEN** the daemon status area includes a switch labeled “退出 ACECode 后继续运行后台进程”

#### Scenario: Browser-only Web UI
- **WHEN** the same Web UI is opened without the native Desktop bridge
- **THEN** it does not present a functional background-continuation switch

### Requirement: Runtime cleanup is generation-safe
Managed runtime cleanup MUST remove state only when the exiting process still owns the recorded PID and GUID generation.

#### Scenario: Old process cleans up after replacement
- **WHEN** an older generation finishes teardown after a newer generation has written runtime state
- **THEN** the older generation leaves the newer PID, GUID, port, token, heartbeat, and managed metadata intact

#### Scenario: Rapid Desktop relaunch
- **WHEN** ACECode is quit and relaunched immediately
- **THEN** at most one compatible managed generation becomes active and the new Desktop either attaches to it or starts one replacement

### Requirement: Background continuation is not an operating-system service
The first version SHALL preserve a process only across Desktop application exit and SHALL NOT imply login startup, reboot persistence, or daemon crash restart.

#### Scenario: User enables background continuation
- **WHEN** the enabled process survives an ACECode quit
- **THEN** no login item, launchd service, systemd unit, or crash-restart service is installed
