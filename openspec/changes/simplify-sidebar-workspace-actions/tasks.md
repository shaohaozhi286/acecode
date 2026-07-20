## 1. Focused Frontend Coverage

- [x] 1.1 Extend the sidebar architecture test to require the ellipsis/new-task button order, shared context-menu dispatch, and absence of dedicated rename/remove buttons.

## 2. Workspace Row Implementation

- [x] 2.1 Update `WorkspaceGroup` to open the existing workspace context menu from an ellipsis button without triggering the row click.
- [x] 2.2 Reduce the trailing workspace action cluster to ellipsis then new task while preserving unread state and context-menu callbacks.

## 3. Verification

- [x] 3.1 Run the web unit suite and production build.
- [x] 3.2 Run strict OpenSpec validation and `git diff --check`, then review the scoped diff against the requested interaction.

## 4. Three-Dot Icon Correction

- [x] 4.1 Add the provided three-circle SVG as a regeneration-safe workspace-menu icon and register its `VsIcon` alias.
- [x] 4.2 Switch the workspace menu button and focused architecture test from the four-diamond asset to the dedicated three-dot icon.
- [x] 4.3 Validate the SVG, run frontend tests/build, strict OpenSpec validation, scoped diff checks, and rendered browser QA.
