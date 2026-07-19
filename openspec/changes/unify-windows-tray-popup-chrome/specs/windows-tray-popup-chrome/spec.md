## ADDED Requirements

### Requirement: Windows tray popup chrome is version independent
The Windows desktop tray popup SHALL render its visible surface, rounded corners, and shadow through ACECode-owned per-pixel-alpha rendering and SHALL NOT use an OS-selected class shadow or default DWM corner treatment on the normal custom-popup path.

#### Scenario: Popup opens on Windows 10
- **WHEN** the user opens the ACECode tray menu on a supported Windows 10 system
- **THEN** the popup SHALL use the configured ACECode surface radius, shadow extent, shadow opacity, and shadow offset
- **AND** unsupported Windows 11 DWM attributes SHALL NOT change the result

#### Scenario: Popup opens on Windows 11
- **WHEN** the user opens the same ACECode tray menu at the same effective DPI on Windows 11
- **THEN** its surface and shadow geometry SHALL match the Windows 10 custom rendering
- **AND** Windows 11 SHALL NOT add a second corner, border, or system shadow

### Requirement: Tray popup chrome is DPI aware without changing content layout
The Windows tray popup SHALL convert chrome design constants from DIP to device pixels exactly once using the configured scale factor of the monitor containing the tray anchor, independently from the process DPI-awareness mode, while preserving the existing visible menu width, row heights, and surface anchor.

#### Scenario: Popup opens on a scaled display
- **WHEN** the tray anchor is on a display whose configured scale factor differs from 100%
- **THEN** the corner radius and shadow geometry SHALL scale proportionally with the menu surface
- **AND** the transparent shadow inset SHALL NOT shift the visible surface away from its computed tray anchor

#### Scenario: Windows 10 target display is configured to 100%
- **WHEN** the target display scale is 100% even if the process or primary display reports a 144 system DPI
- **THEN** popup geometry SHALL use 96 DPI
- **AND** the visible menu surface SHALL retain its 280-pixel design width

#### Scenario: Monitor scale query fails
- **WHEN** the target monitor scale factor cannot be read
- **THEN** popup geometry SHALL fall back to 96 DPI
- **AND** SHALL NOT reuse a process-awareness-dependent system DPI

### Requirement: Tray popup font composes display DPI and text size exactly once
The Windows tray popup SHALL calculate its font height from the unscaled design font, the target monitor geometry DPI, and the Windows text-size percentage, applying each scale exactly once.

#### Scenario: Default-size Windows 10 display
- **WHEN** Windows text size is 100% and the target monitor geometry DPI is 96
- **THEN** the popup SHALL create its font at the 13-pixel design height

#### Scenario: 140% 4K display
- **WHEN** Windows text size is 100% and the target monitor reports 140% scale, producing 134 geometry DPI
- **THEN** the popup SHALL create an 18-pixel font
- **AND** text measurement and rendering SHALL use that same font

#### Scenario: 150% display
- **WHEN** Windows text size is 100% and the target monitor geometry DPI is 144
- **THEN** the popup SHALL create a 20-pixel font

#### Scenario: Accessibility text size is enlarged
- **WHEN** Windows text size is greater than 100%
- **THEN** the monitor-DPI-scaled popup font SHALL additionally scale by that text-size percentage exactly once
- **AND** text rows SHALL expand when required to prevent vertical clipping

#### Scenario: Windows text-size setting is unavailable
- **WHEN** the text-size value cannot be read or is outside the supported range
- **THEN** the popup SHALL use the default 100% text size

### Requirement: Shadow pixels are not interactive menu content
The expanded layered window SHALL treat every point outside the rounded white menu surface as transparent for hit testing, including partially visible shadow pixels.

#### Scenario: User clicks through the shadow
- **WHEN** the pointer is over the popup shadow but outside the rounded menu surface
- **THEN** that point SHALL NOT activate a menu row
- **AND** normal click-away dismissal behavior SHALL remain available

#### Scenario: User interacts with menu content
- **WHEN** the pointer or keyboard targets a selectable row inside the rounded surface
- **THEN** hover, scrolling, submenu opening, and command dispatch SHALL retain their existing behavior

### Requirement: Main popup and submenus share one chrome implementation
The main Windows tray popup and every custom `More` submenu SHALL use the same layered surface renderer, DPI-scaled chrome constants, and transparent hit-testing rules.

#### Scenario: More submenu opens
- **WHEN** the user opens a `More` row from either the Pinned or Recent section
- **THEN** the submenu SHALL use the same corner and shadow treatment as the main popup
- **AND** submenu placement SHALL be calculated from the main popup's visible surface rather than its transparent shadow bounds

### Requirement: Native fallback remains available
The Windows tray implementation SHALL retain the native menu fallback when the initial custom layered popup cannot be created or rendered.

#### Scenario: Initial layered rendering fails
- **WHEN** window creation, DIB allocation, or the first layered-window update fails
- **THEN** ACECode SHALL destroy the incomplete custom popup
- **AND** SHALL show the existing native functional fallback menu
