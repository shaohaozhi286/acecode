## Why

Each workspace row currently exposes three separate action buttons for creating a task, renaming the workspace, and removing it. This makes the row visually busy and duplicates actions that are already available from the workspace context menu.

## What Changes

- Replace the three workspace-row action buttons with two compact buttons in this order: an ellipsis menu button followed by the existing workspace-scoped new-task button.
- Render the workspace ellipsis as three horizontally aligned circular dots, using a dedicated monochrome SVG rather than the existing four-diamond `Ellipsis` asset.
- Make a left click on the ellipsis button open the same workspace context menu, with the same target and actions, as a right click on the workspace row.
- Keep rename, remove, and all other workspace operations available through the shared context menu instead of rendering dedicated row buttons.

## Capabilities

### New Capabilities

- `sidebar-workspace-actions`: Defines the compact workspace-row action layout and shared left-click/right-click context-menu behavior.

### Modified Capabilities

None.

## Impact

- Affects the workspace-row controls in `web/src/components/Sidebar.jsx` and focused frontend architecture coverage.
- Reuses the existing desktop context-menu target attributes, menu construction, action dispatch, and confirmation behavior.
- Does not change daemon APIs, session creation semantics, workspace persistence, or dependencies.
