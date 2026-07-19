## Why

The Windows tray popup still delegates its outer shadow and part of its corner treatment to OS-managed window chrome. Windows 10 and Windows 11 therefore render the same ACECode menu with visibly different corners and shadows, despite the menu content already being custom drawn.

## What Changes

- Make the normal Windows tray popup path render its surface, anti-aliased corners, and shadow into one ACECode-owned layered window instead of using `CS_DROPSHADOW`.
- Use one explicit rounded-surface geometry on supported Windows versions, avoiding the current Win32 radius/diameter ambiguity.
- Suppress version-specific DWM corner and border decoration so Windows 10 and Windows 11 use the same popup geometry.
- Apply the same chrome path to the main tray popup and its `More` submenus.
- Compose popup typography from the popup window's coordinate-space DPI and the Windows text-size setting: the 13-pixel design font stays 13 pixels at 96 DPI/100% text size and reaches the same physical size whether Windows or ACECode performs the monitor scaling.
- Prefer a per-monitor thread context for the popup. If Windows or a compatibility policy leaves the thread system-aware or unaware, use that context's system/96 DPI instead of manually applying the target monitor DPI before Windows virtualizes the window again.
- Read the target monitor's configured scale factor independently from the process-awareness-dependent `GetDpiForMonitor` result, while using it directly only in a per-monitor coordinate space.
- Add focused regression coverage for deterministic popup chrome geometry and retain the native menu only as a creation-failure fallback.

## Capabilities

### New Capabilities

- `windows-tray-popup-chrome`: Defines deterministic, DPI-aware tray popup corners and shadows across supported Windows versions.

### Modified Capabilities

None.

## Impact

- `src/desktop/tray_icon_win.cpp`: Windows popup window creation, shape, shadow painting, positioning, and cleanup.
- `src/desktop/tray_menu_popup_model.hpp`: Pure geometry and text-scale helpers used by the Win32 renderer.
- `tests/desktop/tray_menu_popup_model_test.cpp`: Cross-version-independent geometry and font-size regression tests.
- No public API, payload schema, menu command, or non-Windows tray behavior changes.
