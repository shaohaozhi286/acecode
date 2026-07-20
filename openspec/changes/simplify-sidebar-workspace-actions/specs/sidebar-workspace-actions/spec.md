## ADDED Requirements

### Requirement: Compact workspace-row actions
Each workspace row in the desktop sidebar SHALL expose exactly two action buttons in its trailing action cluster: an ellipsis menu button followed by a workspace-scoped new-task button. The ellipsis glyph SHALL consist of three horizontally aligned circular dots and SHALL NOT use the four-diamond glyph. The row SHALL NOT expose dedicated rename or remove buttons.

#### Scenario: Workspace row actions become visible
- **WHEN** pointer hover or keyboard focus reveals the actions for a workspace row
- **THEN** the first action button is the workspace menu button rendered as three horizontally aligned circular dots
- **AND** the button immediately to its right is the workspace-scoped new-task button
- **AND** no dedicated rename or remove action button is rendered

### Requirement: Ellipsis reuses the workspace context menu
A left click on the workspace ellipsis button SHALL open the same shared workspace context menu for the same workspace target that a right click on the workspace row opens. Opening the menu SHALL NOT activate, expand, or collapse the row as a side effect of the initiating click.

#### Scenario: Open workspace menu with ellipsis
- **WHEN** the user left-clicks a workspace row's ellipsis button
- **THEN** ACECode opens the shared workspace context menu anchored to that button
- **AND** the menu derives its actions and enabled states from that workspace row's context target
- **AND** the workspace row is not activated, expanded, or collapsed by that click

#### Scenario: Right-click behavior remains equivalent
- **WHEN** the user right-clicks the same workspace row
- **THEN** ACECode opens the same shared workspace context menu with the same workspace actions and enabled states

### Requirement: Workspace new-task shortcut remains direct
The new-task button to the right of the ellipsis SHALL continue to start a new task scoped to that workspace without opening the workspace context menu or activating the row.

#### Scenario: Create task from compact action cluster
- **WHEN** the user clicks the workspace new-task button
- **THEN** ACECode starts the existing workspace-scoped new-task flow for that workspace
- **AND** the shared workspace context menu is not opened
