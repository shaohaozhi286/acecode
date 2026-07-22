## Context

The Desktop product is a native C++ shell around the same React/Vite UI served by the daemon. React currently mounts without a locale provider, the HTML root is permanently tagged `zh-CN`, presentation helpers contain Chinese text and hand-built date/count formatting, and native tray/dialog code owns another set of mixed Chinese/English labels. `acecode.uiPrefs.v1` and theme preferences are browser-local, while the native shell already loads `~/.acecode/config.json` before creating the WebView and can inject startup JavaScript before application modules execute.

The change must preserve unrelated user-authored content, keep the existing offline embedded-asset model, work in Desktop shell and ordinary browser modes, and avoid making the native shell parse or depend on the full React catalog. Existing installations without a locale field must not silently change language.

## Goals / Non-Goals

**Goals:**

- Provide complete `zh-CN` and `en-US` product chrome for the embedded WebUI and native Desktop surfaces.
- Resolve the effective locale before first render in the Desktop shell and switch both layers without restarting the application.
- Persist one GUI locale preference shared by Desktop and browser WebUI while preserving legacy Chinese behavior.
- Use locale-aware date, relative-time, number, and plural formatting.
- Define a stable boundary between localizable product presentation data and opaque user/model/tool content.
- Enforce catalog completeness and localized native behavior through automated tests.

**Non-Goals:**

- Localizing the terminal TUI, CLI help, logs, protocol keys, source code, terminal output, model responses, user messages, file contents, workspace/session titles, or third-party/custom metadata.
- Adding right-to-left language layouts in this release; the locale contract and document direction update remain extensible.
- Downloading translations at runtime or adding an online translation service.

## Decisions

### 1. Persist `ui.locale`, resolve a separate effective locale

`AppConfig` gains `UiConfig { locale }` with supported preference values `auto`, `zh-CN`, and `en-US`. Existing config files that omit `ui.locale` resolve to the legacy `zh-CN` preference. The generated default config for a new installation writes `ui.locale: auto`; `auto` resolves Chinese system locales to `zh-CN` and all other/unknown locales to `en-US`.

The daemon exposes authenticated `GET` and `PUT /api/config/ui-locale` routes. The response contains the stored preference; native Desktop resolves `auto` from the OS locale, while a browser resolves it from `navigator.language`. This is canonical persistence. `localStorage` caches the last preference only to avoid first-paint flicker and is never authoritative once the daemon responds.

Alternative considered: adding locale only to `acecode.uiPrefs.v1`. Rejected because native startup dialogs and the tray exist before and outside React, and browser-local storage cannot provide one application-wide setting.

### 2. Resolve Desktop locale before module evaluation

The native shell loads the config before WebView navigation, resolves `auto` through platform locale helpers, and injects `window.__ACECODE_LOCALE_PREFERENCE__` plus `window.__ACECODE_LOCALE__` through the existing `WebHost::init_script` seam. React initializes synchronously from those globals before mounting, preventing a Chinese-to-English first-frame flash.

When Settings changes locale, React performs an optimistic `i18n.changeLanguage`, persists through the daemon API, and invokes `aceDesktop_applyLocale` when present. The native bridge updates in-memory locale state used by subsequent tray/dialog/notification presentations. The save and bridge sequence is transactional from the UI's perspective: either both layers accept it, or the daemon preference, native state, and React locale are rolled back. A successful change schedules a lightweight WebView page reload so module-scope presentation maps are reevaluated without restarting ACECode or its daemon.

Alternative considered: encoding locale in the initial URL. Rejected because the existing token query is immediately consumed and rewritten, and locale is application state rather than navigation state.

### 3. Use synchronous bundled i18next catalogs

The WebUI uses `i18next` with `react-i18next`. Catalogs are imported into the Vite bundle rather than fetched from `/locales`, preserving the offline single-package behavior and avoiding Suspense/loading states. Frequently reused and grammar-sensitive copy uses reviewed semantic keys. The existing large static presentation surface is covered by a build-time Babel compiler: it extracts static Han-script product copy, assigns content-addressed keys, and rewrites only catalogued literals to `tr(...)` during Vite builds. The generated `zh-CN`/`en-US` source catalog is committed and runtime behavior is fully offline. `zh-CN` remains the emergency fallback, while tests require both supported catalogs to be complete so fallback text is not expected in normal English use.

React's root subscribes through `useTranslation`, and presentation helpers call the initialized application instance at execution time. The locale transaction reloads the WebView page after a successful switch so existing module-level presentation constants and maps are reconstructed in the new language. This keeps the migration comprehensive without introducing a permanent DOM-rewriting layer.

Alternative considered: a DOM MutationObserver that replaces rendered Chinese text. Rejected because it would fight React reconciliation, miss notifications and non-DOM strings, and blur the boundary between product copy and user content.

### 4. Keep native localization deliberately small

Native-only presentation uses a typed `DesktopLocale` resolver and a small `DesktopStrings` lookup covering tray fixed entries, About labels, folder picker text, compatibility/startup failures, and native fallback notifications. It does not parse React JSON catalogs. Native functions receive the locale explicitly where practical; process-wide mutable locale state is restricted to the shell coordinator that owns tray/dialog callbacks.

Desktop WebUI notifications already originate in JavaScript and therefore use WebUI translations. The shared C++ completion-notification builder accepts an optional locale; its default remains `zh-CN`, preserving legacy callers while native Desktop can request English shell text.

### 5. Localize product metadata, preserve opaque content

Known built-in command descriptions and known error codes are presentation data. Built-in command payloads gain stable IDs and the client maps those IDs to catalog keys; unrecognized/custom command descriptions remain exactly as authored. APIs continue to return machine-readable codes plus diagnostic fallback messages; the client localizes known codes and only exposes the fallback for unknown cases.

Session titles, workspace names, Skill/connector/custom-command descriptions, model content, terminal output, file contents, paths, tool arguments/results, and user-authored templates are never passed through a translator.

### 6. Make formatting locale-aware

Shared helpers use `Intl.DateTimeFormat`, `Intl.RelativeTimeFormat`, and `Intl.NumberFormat` with the effective locale. Count phrases use i18next plural rules and interpolation rather than concatenating Chinese measure words. Search case-folding and path sorting retain locale-neutral behavior unless display sorting explicitly receives the selected locale.

### 7. Validate coverage at catalog and presentation boundaries

Tests compare semantic and generated catalog topology plus interpolation placeholders between `zh-CN` and `en-US`, exercise locale normalization/resolution and config persistence, verify runtime switching and bridge payloads, and cover native strings for both locales. The source compiler test requires every static Han-script product string to be catalogued or explicitly opaque. Four LOOP prompt bodies are the only source allowlist entries because they are user-editable task content rather than product chrome. New generated-catalog entries fail offline with an explicit review list unless a contributor deliberately opts into machine-translated drafts.

Visual QA covers light/dark themes, narrow Desktop windows, 100/125/150 percent scale, tray layout, dialogs, notifications, guided tour, and long English labels.

## Risks / Trade-offs

- **[Large migration surface causes missed strings]** → Migrate by product surface, add a source audit and catalog parity gate, and manually exercise the full settings/navigation/chat matrix in both locales.
- **[Desktop and daemon hold separate in-memory config copies]** → The daemon API is the sole disk writer for runtime locale changes; the native bridge applies only in-memory presentation state and reloads config on the next process start.
- **[English text overflows fixed Chinese-sized controls]** → Prefer flexible layout/ellipsis already used by ACECode and include narrow-window and DPI visual checks before completion.
- **[Unknown backend text remains mixed-language]** → Localize only stable codes/IDs and retain diagnostic fallback text; do not heuristically translate arbitrary messages.
- **[New dependencies affect offline builds]** → Pin them in the existing pnpm lockfile and bundle all resources into `web/dist`.

## Migration Plan

1. Add locale primitives, catalogs, config/API, and startup injection while retaining Chinese as the legacy missing-field behavior.
2. Add the Settings selector and migrate core shell/navigation/chat surfaces; keep all existing behavioral tests green.
3. Migrate remaining presentation helpers and native surfaces, then enable missing-key/source-audit gates.
4. Build `web/dist`, reconfigure CMake, build and test the Desktop binary, and manually validate both languages.
5. Rollback is a normal code rollback: old binaries ignore the new `ui` config object, and removing the field restores legacy Chinese behavior.

## Open Questions

None. Initial locales, legacy behavior, new-install default, GUI-only scope, and opaque-content boundaries are fixed by this change.
