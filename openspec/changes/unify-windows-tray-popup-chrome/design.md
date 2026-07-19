## Context

The tray menu body is already custom drawn, but its `WS_POPUP` window still uses `CS_DROPSHADOW`, `SetWindowRgn`, and Windows 11-only DWM attributes. On Windows 11 the compositor supplies modern rounded chrome; on Windows 10 the DWM attributes are unsupported and the class shadow falls back to legacy system styling. The existing `CreateRoundRectRgn` call also passes the configured radius as the ellipse diameter, halving the intended corner.

The menu must retain its current payload, two-column session rows, `More` submenus, keyboard handling, DPI behavior, and native-menu emergency fallback. The supported Windows toolchain already links `user32` and `gdi32`, which provide the required layered-window APIs.

## Goals / Non-Goals

**Goals:**

- Render the same popup surface, corner radius, and shadow on Windows 10 and Windows 11.
- Keep geometry in DIPs and scale it exactly once from the configured scale factor of the monitor containing the tray anchor.
- Keep the font at its intended 13-pixel height only at 96 DPI/100% text size, then apply monitor DPI and Windows text-size scaling exactly once each.
- Preserve current mouse, keyboard, scrolling, submenu, command-dispatch, and fallback behavior.
- Keep transparent shadow pixels outside the interactive menu surface.

**Non-Goals:**

- Redesign menu content, typography, row ordering, or labels.
- Change Linux or macOS tray backends.
- Remove the native `TrackPopupMenu` creation-failure fallback.
- Add a third-party graphics dependency.

## Decisions

### 1. Render one per-pixel-alpha popup instead of pairing a normal window with system chrome

The interactive popup will use `WS_EX_LAYERED`. A top-down 32-bit DIB will contain the software-rendered shadow and anti-aliased white rounded surface; existing GDI text and row rendering will then draw into the opaque surface area before the pixels are premultiplied and sent through `UpdateLayeredWindow`.

This keeps the surface and shadow in one z-ordered window, so submenus cannot separate from companion shadow windows. A separate shadow window was considered, but it would leave binary `SetWindowRgn` edges and introduce owner/z-order synchronization complexity.

### 2. Treat the existing popup position as the surface position

Pure chrome geometry will expand the actual HWND around the existing surface rectangle by a DPI-scaled transparent/shadow inset. Anchor calculations, menu width, viewport height, and submenu placement continue to refer to the visible white surface, so the screenshot-proven content layout does not move.

Mouse row lookup subtracts the chrome inset. Rounded-corner and shadow-only points return `HTTRANSPARENT`, while points inside the rounded white surface remain normal client input.

### 3. Fully suppress OS-selected popup chrome

The popup class will no longer request `CS_DROPSHADOW`. When DWM attributes are available, the window will request `DWMWCP_DONOTROUND` and no system border; unsupported calls on Windows 10 remain harmless. ACECode's alpha surface is therefore the only visible corner and shadow path on both systems.

### 4. Keep rendering constants deterministic and monitor-scale-aware

Corner radius, shadow extent, vertical shadow offset, blur falloff, and maximum opacity are fixed design constants converted from DIP once through the target monitor's configured scale factor. Software coverage uses rounded-rectangle distance, producing the same pixel geometry for the same configured scale regardless of OS version, process DPI-awareness fallback, or desktop effects policy.

### 5. Keep the native fallback scoped to real custom-popup failure

The first layered render is part of custom popup creation. If the window, DIB, or initial `UpdateLayeredWindow` operation fails, the custom window is destroyed and the existing native menu fallback is used. A later transient repaint failure leaves the previous layered pixels visible rather than changing menu backend mid-interaction.

### 6. Compose display scaling and Windows text-size scaling

Popup width, padding, row geometry, corners, shadow, and the Segoe UI font all start from design values at 96 DPI. The font additionally honors the independent Windows text-size percentage. Its pixel height is therefore calculated in one step as `round(13 × geometryDpi / 96 × textScalePercent / 100)`.

At 96 DPI/100% text size the font remains 13 pixels. The observed 4K monitor reports 140% (`geometryDpi = 134`), so the same 100% text-size setting produces an 18-pixel font instead of the incorrect 13-pixel font. A 150% display produces 20 pixels. The text-size percentage is read whenever the tray popup opens, so a settings change is reflected without restarting ACECode. Values are constrained to Windows' supported 100%-225% range and fall back to 100% when unavailable. Text rows keep their existing DPI-scaled height unless the composed font height requires more vertical space.

### 7. Query the monitor scale directly instead of inferring it from process DPI

`GetDpiForMonitor` changes its result according to the caller's process DPI awareness. On a Windows 10 system-aware fallback it can therefore return the primary/system DPI for a 100% target display and inflate the 280-pixel popup to 420 pixels.

The popup will dynamically call `GetScaleFactorForMonitor` for the monitor containing the tray anchor, then convert that percentage to geometry DPI (`100% → 96`, `125% → 120`, `150% → 144`). The conversion is pure and covered by tests. If the supported API is unexpectedly unavailable or fails, geometry falls back to 100%/96 DPI rather than risking another oversized popup.

## Risks / Trade-offs

- [Software alpha rendering adds work when a popup opens] → The surface is small, rendering happens only on open or visible state changes, and no timer is introduced.
- [GDI drawing can overwrite DIB alpha bytes] → Keep separate surface-coverage and shadow-alpha masks, then restore alpha and premultiply every pixel after all text/row drawing.
- [The expanded HWND could intercept clicks in invisible margins] → Return `HTTRANSPARENT` for every point outside the rounded menu surface.
- [Layered rendering could fail on a constrained machine] → Treat initial render failure as custom-popup creation failure and retain the native menu fallback.
- [Large accessibility text could clip inside the compact row layout] → Use the larger of the existing DPI-scaled row height and the scaled font height plus vertical padding.
- [Display DPI or text scale could be applied twice] → Compute the font height in one pure helper from the unscaled 13-pixel design value, geometry DPI, and text scale, with explicit 96/134/144-DPI tests.
- [Monitor scale query could fail on a constrained Windows image] → Fall back to the compact 96-DPI design geometry and retain functional input/rendering.

## Migration Plan

Ship the renderer as a direct replacement for the current Windows custom popup path. Rollback is limited to reverting this change; payload and command contracts are unchanged and no persisted state migrates.

## Open Questions

None.
