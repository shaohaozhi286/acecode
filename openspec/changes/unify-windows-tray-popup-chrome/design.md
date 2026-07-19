## Context

The tray menu body is already custom drawn, but its `WS_POPUP` window still uses `CS_DROPSHADOW`, `SetWindowRgn`, and Windows 11-only DWM attributes. On Windows 11 the compositor supplies modern rounded chrome; on Windows 10 the DWM attributes are unsupported and the class shadow falls back to legacy system styling. The existing `CreateRoundRectRgn` call also passes the configured radius as the ellipse diameter, halving the intended corner.

The menu must retain its current payload, two-column session rows, `More` submenus, keyboard handling, DPI behavior, and native-menu emergency fallback. The supported Windows toolchain already links `user32` and `gdi32`, which provide the required layered-window APIs.

## Goals / Non-Goals

**Goals:**

- Render the same popup surface, corner radius, and shadow on Windows 10 and Windows 11.
- Keep geometry in DIPs and scale it exactly once for the monitor containing the tray anchor.
- Keep the 100% Windows text-size font at the intended 13-pixel height instead of multiplying it by monitor display DPI.
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

### 4. Keep rendering constants deterministic and DPI-aware

Corner radius, shadow extent, vertical shadow offset, blur falloff, and maximum opacity are fixed design constants converted from DIP once through the existing monitor DPI. Software coverage uses rounded-rectangle distance, producing the same pixel geometry for the same effective DPI regardless of OS version or desktop effects policy.

### 5. Keep the native fallback scoped to real custom-popup failure

The first layered render is part of custom popup creation. If the window, DIB, or initial `UpdateLayeredWindow` operation fails, the custom window is destroyed and the existing native menu fallback is used. A later transient repaint failure leaves the previous layered pixels visible rather than changing menu backend mid-interaction.

### 6. Separate display scaling from Windows text-size scaling

Popup width, padding, row geometry, corners, and shadow remain monitor-DPI-scaled. The Segoe UI font instead starts from a 13-pixel design height and applies only the current Windows text-size percentage. At the default 100% text size this therefore creates a 13-pixel font even when the popup is on a 150% display.

The text-size percentage is read whenever the tray popup opens, so a settings change is reflected without restarting ACECode. Values are constrained to Windows' supported 100%-225% range and fall back to 100% when the setting is unavailable. Text rows keep their existing DPI-scaled height unless an enlarged accessibility font requires more vertical space.

## Risks / Trade-offs

- [Software alpha rendering adds work when a popup opens] → The surface is small, rendering happens only on open or visible state changes, and no timer is introduced.
- [GDI drawing can overwrite DIB alpha bytes] → Keep separate surface-coverage and shadow-alpha masks, then restore alpha and premultiply every pixel after all text/row drawing.
- [The expanded HWND could intercept clicks in invisible margins] → Return `HTTRANSPARENT` for every point outside the rounded menu surface.
- [Layered rendering could fail on a constrained machine] → Treat initial render failure as custom-popup creation failure and retain the native menu fallback.
- [Large accessibility text could clip inside the compact row layout] → Use the larger of the existing DPI-scaled row height and the scaled font height plus vertical padding.

## Migration Plan

Ship the renderer as a direct replacement for the current Windows custom popup path. Rollback is limited to reverting this change; payload and command contracts are unchanged and no persisted state migrates.

## Open Questions

None.
