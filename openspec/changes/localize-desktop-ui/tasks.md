## 1. M1 - Locale foundation and core Desktop shell

- [x] 1.1 Add pinned Web localization dependencies, synchronous `zh-CN`/`en-US` catalog namespaces, locale normalization/resolution helpers, and catalog parity tests
- [x] 1.2 Add `UiConfig::locale` parsing, sparse serialization, generated-new-config default behavior, and C++ config tests
- [x] 1.3 Add authenticated GUI-locale GET/PUT routes, web API bindings, structured validation/persistence behavior, daemon API documentation, and route tests
- [x] 1.4 Add native system-locale detection, pre-mount WebView locale injection, runtime `aceDesktop_applyLocale` bridge state, and pure locale tests
- [x] 1.5 Add the Settings > General language row using existing ACECode card/select styling, optimistic switching, persistence rollback, and native bridge synchronization
- [x] 1.6 Localize application bootstrap metadata, settings navigation/general/appearance, TopBar, Sidebar, home/project selection, composer controls, and input shell

## 2. M2 - Complete WebUI presentation and formatting

- [x] 2.1 Localize chat/transcript, message actions, activity/status, tool blocks, permission/question flows, queue cards, subagent surfaces, and completion summaries
- [x] 2.2 Localize search/find, context menus, side/preview/change panels, file previews, console shell, update flow, dialogs, and empty/error states
- [x] 2.3 Localize settings surfaces for configuration, personalization, skills, MCP, connectors, models, tools, hooks, archived sessions, usage, feedback, and About
- [x] 2.4 Localize loop/schedule UI and presets while keeping prompt bodies opaque, and replace hand-built date, relative-time, number, and count formatting with locale-aware helpers
- [x] 2.5 Localize known error codes, notification shells, permission-mode presentation, built-in command metadata, and other stable backend presentation identifiers while preserving unknown/custom content
- [x] 2.6 Localize the guided tour, accessibility labels, tooltips, placeholders, document metadata, compatibility page copy, and no-script fallback
- [x] 2.7 Run the presentation-literal audit, migrate or explicitly allowlist every remaining Desktop-visible product string, and verify opaque Chinese content remains unchanged in English mode

## 3. M3 - Native Desktop localization and quality gates

- [x] 3.1 Add typed native Desktop string catalogs and localize fixed tray section/action/fallback labels without translating session or workspace titles
- [x] 3.2 Localize native About, folder picker, compatibility fallback, startup/fatal errors, and native fallback notification presentation across supported platform code
- [x] 3.3 Add native unit coverage for both locales, automatic resolution, tray layouts, About formatting, and startup/folder-picker presentation helpers
- [x] 3.4 Add Web tests for runtime switching, settings persistence rollback, native bridge payloads, catalog/interpolation parity, locale-aware formatting, and opaque-content preservation
- [x] 3.5 Document locale configuration, translation contribution rules, localizable-content boundaries, and Desktop embedded-asset rebuild requirements

## 4. Verification and delivery

- [x] 4.1 Install locked Web dependencies and pass the complete `pnpm test` and `pnpm build` gates
- [x] 4.2 Reconfigure CMake, build `acecode_unit_tests` and `acecode-desktop`, and pass focused/full C++ tests without claiming unrelated process-lock failures as regressions
- [x] 4.3 Validate Chinese and English Desktop UI in light/dark themes, narrow window and scaled layouts, tray, About, notifications, guided tour, and settings runtime switching
- [x] 4.4 Pass strict OpenSpec validation and diff checks, confirm the main worktree remains untouched, and commit the completed worktree branch
