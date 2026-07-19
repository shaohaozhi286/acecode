## 1. Deterministic Chrome Geometry

- [x] 1.1 Add pure popup chrome bounds and rounded-surface hit-test helpers.
- [x] 1.2 Add focused tests for shadow insets, preserved surface anchoring, and transparent corner hit testing.

## 2. Layered Popup Rendering

- [x] 2.1 Add DPI-scaled shadow metrics and remove OS-selected class shadow / corner decoration from the custom popup path.
- [x] 2.2 Render the rounded white surface and deterministic alpha shadow into a 32-bit DIB and present it with `UpdateLayeredWindow`.
- [x] 2.3 Adapt existing row, text, separator, hover, and scrollbar drawing to the inset surface and restore premultiplied alpha after GDI drawing.

## 3. Input, Submenus, and Fallback

- [x] 3.1 Expand the HWND around the visible surface while preserving anchor geometry, row lookup, and transparent hit testing.
- [x] 3.2 Position and render `More` submenus from visible surface bounds using the same layered chrome path.
- [x] 3.3 Treat initial layered-render failure as custom-popup creation failure and preserve cleanup plus the native functional fallback.

## 4. Verification

- [x] 4.1 Run focused tray popup/model tests and strict OpenSpec validation.
- [x] 4.2 Build the Windows desktop support target, run applicable tray regressions, and check the final diff for whitespace or unrelated changes.

## 5. Text Scale Follow-up

- [x] 5.1 Add pure helpers and tests for 100%-225% Windows text scaling and row-height accommodation.
- [x] 5.2 Read the current Windows text-size percentage whenever the tray popup opens and use it for font creation and text measurement.
- [x] 5.3 Keep menu geometry monitor-DPI-aware while preventing enlarged accessibility text from clipping.
- [x] 5.4 Build the affected Windows targets, run focused tests, validate OpenSpec strictly, and inspect the final diff.
