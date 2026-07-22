## Why

ACECode Desktop currently exposes a mostly Chinese React interface plus a separate set of hard-coded native shell strings, with no shared locale state or translation quality gate. Supporting Simplified Chinese and English requires one startup-safe locale contract spanning the embedded WebUI, native tray/dialogs, persisted settings, and structured backend presentation data.

## What Changes

- Add a global GUI locale preference with `auto`, `zh-CN`, and `en-US` values, persisted in the ACECode config and editable from Settings > General.
- Resolve the effective locale before the Desktop WebView mounts, while preserving Chinese behavior for existing configs that do not contain a locale preference.
- Add a React localization layer with complete Simplified Chinese and English catalogs, runtime switching, locale-aware dates/relative times/counts, document metadata, accessibility labels, notifications, guided tour, and all Desktop-visible product chrome.
- Localize native Desktop tray entries, About content, folder picker labels, compatibility/startup failures, and other native dialogs on supported platforms.
- Replace presentation-sensitive backend strings with stable identifiers or structured error codes where the Desktop client must localize them; user-authored, model-authored, workspace, terminal, file, Skill, connector, and custom-command content remains unchanged.
- Add catalog parity, locale resolution, persistence, bridge, native formatting, and regression tests, plus contributor and daemon API documentation.
- Keep TUI/CLI interface localization outside this change except where shared code must retain existing behavior.

## Capabilities

### New Capabilities

- `desktop-localization`: Locale selection, persistence, resolution, localized embedded WebUI and native Desktop surfaces, presentation-data boundaries, and translation quality gates.

### Modified Capabilities

None.

## Impact

- Web: `web/src` application bootstrap, Settings, shared presentation helpers, all Desktop-visible React components, locale catalogs, tests, and package dependencies.
- Native Desktop: `src/desktop`, locale detection, WebView bootstrap injection, tray layout, About/folder-picker/startup dialogs, and native unit tests.
- Config/API: `src/config`, authenticated locale configuration routes under `src/web`, API client bindings, and `docs/daemon-api.md`.
- Build/release: localized Web assets remain embedded through `web/dist`, so verification requires the web build followed by CMake reconfiguration and Desktop rebuild.
