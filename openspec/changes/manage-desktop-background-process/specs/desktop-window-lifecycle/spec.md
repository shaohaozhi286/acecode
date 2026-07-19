## ADDED Requirements

### Requirement: Closing the macOS window does not quit ACECode
On macOS, a window close request SHALL hide the ACECode window while leaving the application, menu-bar item, and active managed process running.

#### Scenario: User clicks the window close control
- **WHEN** the user clicks the ACECode window close control
- **THEN** the window becomes hidden and the WebView application loop continues

#### Scenario: Background continuation is disabled
- **WHEN** the user closes the window while “退出 ACECode 后继续运行后台进程” is disabled
- **THEN** the managed process continues because no real application quit occurred

### Requirement: Hidden macOS window can be restored
ACECode SHALL show, activate, and foreground its hidden window when the user chooses the Dock icon or the menu-bar open action.

#### Scenario: Dock reopen
- **WHEN** the ACECode window is hidden and the user selects ACECode in the Dock
- **THEN** the existing window is shown and brought to the foreground

#### Scenario: Menu-bar reopen
- **WHEN** the ACECode window is hidden and the user selects the menu-bar icon's open action
- **THEN** the same existing window is shown and brought to the foreground

### Requirement: Explicit quit bypasses window hiding
An explicit application quit SHALL bypass the close-to-hide handler and complete application teardown before applying the managed process exit policy.

#### Scenario: User selects Quit
- **WHEN** the user invokes Cmd-Q, the native quit bridge, or the menu-bar Quit action
- **THEN** ACECode terminates its WebView loop and native UI and then stops or releases the managed process according to the saved preference
