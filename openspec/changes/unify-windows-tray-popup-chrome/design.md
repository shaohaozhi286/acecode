## Context

The tray menu body is already custom drawn, but its `WS_POPUP` window still uses `CS_DROPSHADOW`, `SetWindowRgn`, and Windows 11-only DWM attributes. On Windows 11 the compositor supplies modern rounded chrome; on Windows 10 the DWM attributes are unsupported and the class shadow falls back to legacy system styling. The existing `CreateRoundRectRgn` call also passes the configured radius as the ellipse diameter, halving the intended corner.

The menu must retain its current payload, two-column session rows, `More` submenus, keyboard handling, DPI behavior, and native-menu emergency fallback. The supported Windows toolchain already links `user32` and `gdi32`, which provide the required layered-window APIs.

## Goals / Non-Goals

**Goals:**

- Render the same popup surface, corner radius, and shadow on Windows 10 and Windows 11.
- Keep geometry in DIPs and scale it exactly once in the popup window's actual coordinate space.
- Prefer a per-monitor popup coordinate space, while remaining correct when an older Windows build or compatibility policy leaves the thread system-aware or unaware.
- Keep the font at its intended 13-pixel height only at 96 DPI/100% text size, then apply coordinate-space DPI and Windows text-size scaling exactly once each.
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

### 4. Keep rendering constants deterministic and coordinate-space-aware

Corner radius, shadow extent, vertical shadow offset, blur falloff, and maximum opacity are fixed design constants converted from DIP once through the DPI represented by the popup's Win32 coordinate space. In a per-monitor context this is the target monitor DPI. In a system-aware context it is the system DPI, and in an unaware context it is 96 DPI; Windows then supplies the remaining monitor transform exactly once. Software coverage uses rounded-rectangle distance, producing the same physical geometry for the same configured scale regardless of OS version, process DPI-awareness fallback, or desktop effects policy.

### 5. Keep the native fallback scoped to real custom-popup failure

The first layered render is part of custom popup creation. If the window, DIB, or initial `UpdateLayeredWindow` operation fails, the custom window is destroyed and the existing native menu fallback is used. A later transient repaint failure leaves the previous layered pixels visible rather than changing menu backend mid-interaction.

### 6. Compose coordinate-space scaling and Windows text-size scaling

Popup width, padding, row geometry, corners, shadow, and the Segoe UI font all start from design values at 96 DPI. The font additionally honors the independent Windows text-size percentage. Its logical pixel height is therefore calculated in one step as `round(13 × layoutDpi / 96 × textScalePercent / 100)`, where `layoutDpi` is the DPI of the popup's current Win32 coordinate space rather than unconditionally the target monitor DPI.

At 96 DPI/100% text size the font remains 13 pixels. In a per-monitor context, the observed 4K monitor reports 140% (`layoutDpi = 134`), so the same 100% text-size setting produces an 18-pixel font instead of the incorrect 13-pixel font. In a system-aware 96-DPI context on a 150% target monitor, ACECode keeps the logical font at 13 pixels and Windows scales the whole popup to the same approximately 20-device-pixel result. The text-size percentage is read whenever the tray popup opens, so a settings change is reflected without restarting ACECode. Values are constrained to Windows' supported 100%-225% range and fall back to 100% when unavailable. Text rows keep their existing DPI-scaled height unless the composed font height requires more vertical space.

### 7. Separate target monitor DPI from the active Win32 coordinate space

`GetDpiForMonitor` changes its result according to the caller's DPI awareness, while `GetScaleFactorForMonitor` reports the target monitor's configured scale independently. Neither value alone says whether Windows will subsequently virtualize a top-level window.

The previous implementation always converted the target scale to DPI and manually applied it. That is correct for a per-monitor-aware popup, but a system-aware or unaware popup on another-DPI monitor is then scaled again by Windows. This is the machine-dependent double scale that made either the Windows 10 machine or the high-DPI Windows 11 machine wrong after each one-sided correction.

The popup will still dynamically call `GetScaleFactorForMonitor` for the target monitor, but will select its layout DPI from the effective thread awareness: target monitor DPI for per-monitor-aware, system DPI for system-aware, and 96 DPI for unaware. The conversion and selection are pure and covered by tests.

### 8. Establish the popup DPI context before reading coordinates

The tray callback will temporarily request `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`, falling back to `PER_MONITOR_AWARE`, before calling `GetCursorPos`, `MonitorFromPoint`, or creating the popup. This makes the anchor, monitor bounds, layered-window bitmap, and window size share one physical coordinate space on supported Windows 10 and Windows 11 systems. The previous thread context is restored before returning from the callback.

The code will still query the effective context instead of assuming the request succeeded. Compatibility settings can reject or override DPI-awareness changes. In that case the awareness-to-layout-DPI rule above preserves one total scale. Diagnostic logging records the target monitor scale, effective layout DPI, owner-window awareness, and created popup-window awareness so another machine can be verified from one log line.

## Risks / Trade-offs

- [Software alpha rendering adds work when a popup opens] → The surface is small, rendering happens only on open or visible state changes, and no timer is introduced.
- [GDI drawing can overwrite DIB alpha bytes] → Keep separate surface-coverage and shadow-alpha masks, then restore alpha and premultiply every pixel after all text/row drawing.
- [The expanded HWND could intercept clicks in invisible margins] → Return `HTTRANSPARENT` for every point outside the rounded menu surface.
- [Layered rendering could fail on a constrained machine] → Treat initial render failure as custom-popup creation failure and retain the native menu fallback.
- [Large accessibility text could clip inside the compact row layout] → Use the larger of the existing DPI-scaled row height and the scaled font height plus vertical padding.
- [Display DPI or text scale could be applied twice] → Select layout DPI from the effective DPI-awareness context, then compute the font from the unscaled 13-pixel design value, layout DPI, and text scale, with explicit per-monitor/system/unaware tests.
- [Monitor scale query could fail on a constrained Windows image] → Fall back to the compact 96-DPI design geometry and retain functional input/rendering.
- [Per-monitor thread context could be unavailable or compatibility-overridden] → Query the effective awareness and use system/96 coordinate-space DPI so Windows owns, rather than duplicates, the remaining monitor transform.

## Migration Plan

Ship the renderer as a direct replacement for the current Windows custom popup path. Rollback is limited to reverting this change; payload and command contracts are unchanged and no persisted state migrates.

## Open Questions

None.
