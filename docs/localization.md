# GUI localization

ACECode localizes the Desktop shell and its embedded WebUI in Simplified Chinese (`zh-CN`) and English (`en-US`). The same WebUI behavior is available when it is opened in a browser. Terminal TUI/CLI output is outside this localization layer.

## Locale preference

`~/.acecode/config.json` stores the canonical preference at `ui.locale`:

- `auto`: use `zh-CN` for a Chinese system/browser locale and `en-US` otherwise.
- `zh-CN`: always use Simplified Chinese.
- `en-US`: always use English.

Newly generated configs contain `"ui": { "locale": "auto" }`. An older config with no locale field retains the historical `zh-CN` behavior. The authenticated `GET` and `PUT /api/config/ui-locale` endpoints read and update the stored preference; unsupported values are rejected without changing it.

Desktop resolves the preference before WebView modules execute and injects both the preference and effective locale. Settings > General applies a change optimistically, persists it through the daemon, and synchronizes the native bridge. Any save or bridge failure rolls all layers back. A successful change updates the current WebUI in place without reloading the embedded page, so draft input, scroll position, open panels, and dialogs are preserved. The confirmed preference is also kept for later navigations in the same WebView session.

## Web catalogs

Web localization is synchronous and bundled. There is no runtime translation request or downloadable locale file.

- `web/src/i18n/catalogs/zh-CN.js` and `en-US.js` contain reviewed semantic keys, reusable terms, and plural-aware count phrases.
- `web/src/i18n/sourceCatalog.generated.js` contains content-addressed mappings for the existing static product-copy surface.
- `web/scripts/localize-static-copy-babel.mjs` rewrites only catalogued static product literals during Vite builds. Function-local copy becomes `tr(...)`; module-scope object properties and array entries become lazy accessors so they resolve against the current locale on every read. Localized modules also invalidate `useMemo` caches when the active language changes.
- `web/scripts/i18n-en-overrides.mjs` holds reviewed English wording for new or context-sensitive source strings.

The compiler rejects unsupported eager module-scope translated primitives. Move those values behind a function or place them in a supported object/array presentation map instead of caching a translated string at module initialization.

Use a semantic key when copy is reused, has plural/interpolation grammar, or represents a stable product identifier. Add the same key and interpolation variables to both semantic catalogs. Static one-off product copy is covered by the source compiler; after adding it, run:

```powershell
cd web
pnpm i18n:catalog
pnpm i18n:audit
pnpm test
pnpm build
```

The catalog command is offline by default. If it reports new strings, add reviewed translations to `scripts/i18n-en-overrides.mjs` and rerun it. Machine-translated drafts require the explicit `--translate-missing` option and still need human review before commit.

Catalog tests enforce key and interpolation parity, verify every detected static product string is catalogued or allowlisted, and reject Han-script text in the English generated catalog.

## Content boundary

Translate ACECode-owned presentation: labels, actions, status text, empty/error states, placeholders, tooltips, accessibility labels, guided-tour steps, notification shells, known error codes, and known built-in command metadata.

Keep these values opaque and byte-for-byte unchanged:

- user messages and user-authored templates;
- model output and tool arguments/results;
- terminal output, file contents, source code, and paths;
- workspace and session titles;
- Skill, connector, model, and custom-command metadata received from users or third parties;
- unknown backend diagnostic fallback text.

Do not pass opaque values to `tr()` or interpolate them into a source string that the compiler will translate. Translate only the surrounding shell. `web/src/i18n/sourceAllowlist.js` currently contains four built-in LOOP prompt bodies; these are task content, not product chrome. Additions to that allowlist require the same explicit product/content-boundary justification.

## Native Desktop strings

Native-only fixed text uses the typed catalog in `src/desktop/strings.hpp` and `strings.cpp`. Add a `DesktopStringId`, add entries at the matching position in both locale arrays, and cover both locales in `tests/desktop`. Session/workspace titles and diagnostic details must remain opaque even when their surrounding native labels are localized.

The process-wide native locale is selected before startup UI and updated by `aceDesktop_applyLocale`. The bridge changes presentation state only; the daemon API remains the sole runtime writer of `config.json`.

## Embedded asset rebuild

Desktop embeds `web/dist` at CMake configure time. After any WebUI or catalog change, the required order is:

1. Run `pnpm test` and `pnpm build` in `web/`.
2. Reconfigure CMake so `static_assets_data.cpp` is regenerated from the new `web/dist`.
3. Rebuild `acecode_unit_tests` and `acecode-desktop`.
4. Run CTest and visually check both locales in the Desktop shell.

Building the Desktop target without rerunning CMake can leave an older WebUI embedded in an otherwise new executable.
