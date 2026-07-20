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

## 6. Windows 10 Geometry Scale Correction

- [x] 6.1 Add a pure monitor-scale-to-DPI conversion with regression tests for 100%, 125%, 150%, and invalid values.
- [x] 6.2 Replace the process-awareness-dependent `GetDpiForMonitor` query with the target monitor's configured scale factor and a compact 96-DPI fallback.
- [x] 6.3 Verify that 100% monitor scale preserves the 280-pixel menu width while higher configured scales still scale geometry exactly once.
- [x] 6.4 Build the affected Windows targets, run focused tests, validate OpenSpec strictly, and inspect the final diff without including unrelated worktree changes.

## 7. High-DPI Typography Correction

- [x] 7.1 Replace the text-only font helper with a single composed display-DPI and text-scale calculation.
- [x] 7.2 Use the composed font height consistently for text measurement and popup rendering.
- [x] 7.3 Add regressions for 96 DPI → 13px, 134 DPI → 18px, 144 DPI → 20px, and combined accessibility scaling.
- [x] 7.4 Build the affected Windows targets, run focused tests, validate OpenSpec strictly, and inspect the final diff without including unrelated worktree changes.

## 8. DPI Awareness Coordinate-Space Correction

- [x] 8.1 Add a pure awareness-to-layout-DPI helper and regressions for per-monitor, system-aware, unaware, and invalid contexts.
- [x] 8.2 Enter a per-monitor thread DPI context before reading the tray anchor and keep it through custom popup creation, restoring the previous context afterward.
- [x] 8.3 Fall back to the effective thread coordinate-space DPI when per-monitor mode is unavailable, and log target, layout, owner, and popup DPI contexts for machine-to-machine diagnosis.
- [x] 8.4 Build the affected Windows targets, run focused tests, validate OpenSpec strictly, and inspect the final diff without including unrelated worktree changes.

## 9. Effective Monitor DPI Source Correction

- [x] 9.1 Prefer `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` for the target monitor after the per-monitor thread context is active; keep `GetScaleFactorForMonitor` only as a last-resort fallback.
- [x] 9.2 Add a pure effective-DPI → scale-percent helper for diagnostics and regressions (96→100, 120→125, 144→150, 134→140).
- [x] 9.3 Document why DPI-aware callers must not treat `GetScaleFactorForMonitor` as authoritative (enum snaps / inflated factors on 100% Win10 displays).
- [x] 9.4 Build `acecode_unit_tests` / `acecode_desktop_support` and run focused tray popup tests (22/22). Full `acecode-desktop.exe` link needs a closed running instance; cross-machine 100% display check remains manual.
