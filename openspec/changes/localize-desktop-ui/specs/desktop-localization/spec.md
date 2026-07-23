## ADDED Requirements

### Requirement: Supported GUI locale preference
The system SHALL support GUI locale preferences `auto`, `zh-CN`, and `en-US`, SHALL persist the preference in the ACECode application config, and SHALL reject unsupported values without changing the stored preference.

#### Scenario: User selects an explicit locale
- **WHEN** the user selects Simplified Chinese or English in Settings
- **THEN** the selected preference is persisted and returned by the locale configuration API

#### Scenario: Unsupported locale is submitted
- **WHEN** a client submits a locale outside the supported preference set
- **THEN** the API returns a structured validation error and preserves the previous preference

### Requirement: Legacy and automatic locale resolution
The system SHALL preserve `zh-CN` for an existing config that lacks a GUI locale field, SHALL write `auto` for a newly generated default config, and SHALL resolve `auto` to `zh-CN` for Chinese system locales and `en-US` otherwise.

#### Scenario: Existing config has no locale field
- **WHEN** ACECode loads a pre-localization config without `ui.locale`
- **THEN** the effective GUI locale remains `zh-CN`

#### Scenario: New installation follows a Chinese system
- **WHEN** a newly generated config contains `ui.locale: auto` and the system locale is Chinese
- **THEN** the effective GUI locale is `zh-CN`

#### Scenario: Automatic preference on another system locale
- **WHEN** the stored preference is `auto` and the system locale is not Chinese or cannot be detected
- **THEN** the effective GUI locale is `en-US`

### Requirement: Locale is available before Desktop first render
The Desktop shell SHALL resolve and inject the stored and effective locales before Web application modules execute.

#### Scenario: English Desktop startup
- **WHEN** the effective locale is `en-US` and the Desktop WebView navigates to the embedded UI
- **THEN** the first rendered product text, document language, and startup metadata are English without an intermediate Chinese frame

### Requirement: Runtime locale switching
The WebUI SHALL switch locale without restarting the application, persist the choice through the daemon, and notify the native Desktop shell when its bridge is available.

#### Scenario: Successful runtime switch
- **WHEN** a Desktop user changes the language in Settings and persistence succeeds
- **THEN** visible WebUI product text and subsequent native tray/dialog text use the new locale during the same application run without reloading the Web document or discarding current input, scroll, panel, and dialog state

#### Scenario: Persistence fails
- **WHEN** saving a requested locale fails
- **THEN** the WebUI restores the previous locale and shows a localized error

#### Scenario: Browser mode has no native bridge
- **WHEN** a browser WebUI changes locale without Desktop bridge functions
- **THEN** the WebUI persists and applies the locale without a bridge error

### Requirement: Complete localized product presentation
Every Desktop-visible ACECode product label, action, status, empty state, error mapping, accessibility label, tooltip, placeholder, guided-tour step, notification shell, and fixed native tray/dialog string SHALL have complete `zh-CN` and `en-US` translations.

#### Scenario: English product walkthrough
- **WHEN** the effective locale is `en-US` and a user visits home, chat, settings, search, update, context-menu, loop, integration, model, feedback, About, and guided-tour surfaces
- **THEN** all ACECode-owned presentation text on those surfaces is English

#### Scenario: Chinese product walkthrough
- **WHEN** the effective locale is `zh-CN` and the same surfaces are visited
- **THEN** the existing intended Chinese product wording remains available

### Requirement: Opaque content remains unchanged
The localization layer MUST NOT translate or mutate user-authored content, model output, terminal output, file contents, source code, paths, workspace/session titles, tool arguments/results, Skill or connector metadata, or custom command descriptions.

#### Scenario: English UI displays Chinese user content
- **WHEN** the GUI locale is English and a session contains Chinese user/model text, a Chinese path, or a Chinese custom Skill description
- **THEN** that opaque content is rendered byte-for-byte unchanged while surrounding product chrome is English

### Requirement: Locale-aware formatting
The WebUI SHALL format dates, relative times, numbers, and count phrases using the effective locale and language-appropriate plural rules.

#### Scenario: Singular and plural English counts
- **WHEN** an English UI renders one file and multiple files
- **THEN** it uses grammatically correct singular and plural phrases

#### Scenario: Chinese relative time
- **WHEN** a Chinese UI renders a recent timestamp
- **THEN** it uses the Chinese locale formatting without English units

### Requirement: Structured localizable backend presentation
Backend responses intended for localized product presentation SHALL expose stable identifiers or error codes, while arbitrary/custom descriptions SHALL remain opaque fallback content.

#### Scenario: Known built-in command
- **WHEN** the command API returns a known built-in command identifier
- **THEN** the WebUI displays the catalog translation for the active locale

#### Scenario: Unknown backend error
- **WHEN** an API error code is not present in the catalog
- **THEN** the client displays the diagnostic fallback message without attempting heuristic translation

### Requirement: Native Desktop localization
The native Desktop shell SHALL localize fixed tray entries, About labels, folder picker labels, compatibility/startup failures, and native fallback dialogs for both supported locales.

#### Scenario: Native tray changes language
- **WHEN** the running Desktop locale changes from Chinese to English
- **THEN** the next tray menu presentation uses English fixed entries without changing session/workspace titles

#### Scenario: Startup fails before WebUI load
- **WHEN** Desktop startup fails before React mounts
- **THEN** the native error dialog uses the resolved effective locale

### Requirement: Translation quality gates
The repository SHALL provide automated checks for catalog key parity, interpolation parity, supported locale resolution, persistence, runtime bridge behavior, and localized native formatting.

#### Scenario: Translation key is missing
- **WHEN** one supported locale omits a required key or interpolation variable
- **THEN** the web test suite fails with the missing namespace/key information

#### Scenario: Localized build verification
- **WHEN** the change is prepared for completion
- **THEN** web tests/build, C++ unit tests, CMake Desktop build, strict OpenSpec validation, and documented visual checks complete successfully
