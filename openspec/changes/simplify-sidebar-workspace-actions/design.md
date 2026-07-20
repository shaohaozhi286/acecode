## Context

`WorkspaceGroup` in `web/src/components/Sidebar.jsx` renders the workspace row, its hover/focus action cluster, and the data attributes consumed by the shared desktop context-menu layer. The row currently renders dedicated new-task, rename, and remove buttons even though `DesktopContextMenu` already derives the full workspace menu from those row attributes and dispatches the selected operation back through `DESKTOP_CONTEXT_ACTION_EVENT`.

The frontend already uses a proven pattern in `ChatView`: a visible ellipsis button dispatches a synthetic, bubbling `contextmenu` event from its context-bearing target so the central context-menu implementation owns item construction, positioning, confirmation, and action dispatch.

## Goals / Non-Goals

**Goals:**

- Render only two workspace-row action buttons, ordered as ellipsis then workspace-scoped new task.
- Make the ellipsis button open the same workspace context menu as a row right click.
- Preserve existing menu operations, row activation/expansion, unread indication, and task creation behavior.

**Non-Goals:**

- Changing the set, labels, ordering, or confirmation rules of workspace context-menu actions.
- Changing workspace or session APIs, persistence, or desktop bridge behavior.
- Redesigning sidebar section-level actions or session-row controls.

## Decisions

### Reopen the shared menu through the existing `contextmenu` path

The ellipsis click handler will stop the original click from reaching the workspace row, calculate an anchor from the button rectangle, and dispatch a bubbling/cancelable `MouseEvent('contextmenu')` from the button. Because the button remains inside the row that owns `data-desktop-workspace-*`, `contextTargetsFromElement` resolves the identical workspace target used by a physical right click.

This is preferred over building a second popover or directly dispatching individual workspace actions because it keeps menu composition, disabled states, destructive confirmation, and future menu additions in one implementation.

### Keep menu callbacks and target attributes on `WorkspaceGroup`

Removing the dedicated rename and remove buttons does not remove `onRename`, `onRemove`, or their context-action event handling. The shared menu still needs those callbacks, and `data-desktop-workspace-remove` must continue to describe whether removal is available.

### Preserve compact hover and keyboard-focus presentation

The action cluster remains in the trailing workspace-row slot and retains the current hover/focus reveal behavior and 16-pixel icon scale. The unread dot remains an indicator rather than an action and will sit outside the two-button sequence so the actionable order is unambiguous.

### Use a dedicated three-dot workspace-menu icon

The workspace menu button will use a dedicated monochrome `WorkspaceMenu.svg` containing three horizontally aligned circular dots at the user-provided 16-by-16 geometry. `Icon.jsx` will expose a workspace-specific alias, and `scripts/regenerate_web_icons.mjs` will own the custom SVG source so regeneration preserves it. The shared `Ellipsis.svg` remains unchanged because it is also used by the separate session-menu control.

## Risks / Trade-offs

- [Synthetic context-menu events could accidentally toggle or activate the workspace row] -> Stop propagation and prevent default on the initiating click, matching the existing session-menu button pattern.
- [Menu position could extend beyond the viewport] -> Anchor near the ellipsis button and rely on the existing context-menu clamping logic.
- [Removing direct rename/remove buttons adds one click for those operations] -> This is the requested simplification, while the complete shared menu remains available from either ellipsis click or row right click.
- [A later icon regeneration could restore the old four-diamond glyph] -> Register the dedicated SVG in the regeneration source of truth and verify generated output.
