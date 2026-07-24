import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function cssRuleBody(styles, selector) {
  const marker = `${selector} {`;
  const start = styles.indexOf(marker);
  assert.notEqual(start, -1, `missing CSS selector: ${selector}`);
  const bodyStart = start + marker.length;
  const end = styles.indexOf('\n}', bodyStart);
  assert.notEqual(end, -1, `unterminated CSS selector: ${selector}`);
  return styles.slice(bodyStart, end);
}

run('ThemeProvider adds color theme without changing the dark-mode contract', () => {
  const theme = source('theme.jsx');

  assert.match(theme, /const STORAGE_KEY = 'ace\.theme';/);
  assert.match(
    theme,
    /usePreference\(STORAGE_KEY, systemThemeFallback\(\), isValidTheme\)/,
  );
  assert.match(
    theme,
    /setTheme\(\(t\) => \(t === 'dark' \? 'light' : 'dark'\)\)/,
  );
  assert.match(
    theme,
    /setTheme\(t === 'dark' \? 'dark' : 'light'\)/,
  );
  assert.match(theme, /COLOR_THEME_STORAGE_KEY,[\s\S]*DEFAULT_COLOR_THEME,[\s\S]*isValidColorTheme/);
  assert.match(
    theme,
    /value=\{\{ theme, colorTheme, toggle, set, setColorTheme \}\}/,
  );
});

run('ThemeProvider applies both root attributes before native background sync', () => {
  const theme = source('theme.jsx');
  const modeAttribute = theme.indexOf("setAttribute('data-theme', theme)");
  const colorAttribute = theme.indexOf("setAttribute('data-color-theme', colorTheme)");
  const nativeSync = theme.indexOf('pushWindowBackgroundColor();', colorAttribute);

  assert.ok(modeAttribute >= 0);
  assert.ok(colorAttribute > modeAttribute);
  assert.ok(nativeSync > colorAttribute);
  assert.match(theme, /\}, \[colorTheme, theme\]\);/);
});

run('Appearance settings separate color cards from the dark-mode toggle', () => {
  const settings = source('components/SettingsPage.jsx');

  assert.match(
    settings,
    /const COLOR_THEME_OPTIONS = \[\s*\{ key: 'blue', label: '蓝色' \},\s*\{ key: 'orange', label: '橙色' \},\s*\];/,
  );
  assert.match(settings, /const active = colorTheme === opt\.key;/);
  assert.match(settings, /onClick=\{\(\) => setColorTheme\(opt\.key\)\}/);
  assert.match(settings, /<div className="text-\[13px\] font-medium">暗黑模式<\/div>/);
  assert.match(
    settings,
    /<Toggle\s+on=\{theme === 'dark'\}\s+onChange=\{\(enabled\) => setTheme\(enabled \? 'dark' : 'light'\)\}/,
  );
  assert.doesNotMatch(settings, /\{ key: 'light', label: '浅色'/);
  assert.doesNotMatch(settings, /\{ key: 'dark', label: '深色'/);
});

run('global tokens define unchanged blue and complete orange light-dark pairs', () => {
  const styles = source('styles/globals.css');
  const blueLight = cssRuleBody(styles, ':root');
  const blueDark = cssRuleBody(styles, '[data-theme="dark"]');
  const orangeLight = cssRuleBody(
    styles,
    ':root[data-color-theme="orange"]:not([data-theme="dark"])',
  );
  const orangeDark = cssRuleBody(
    styles,
    ':root[data-color-theme="orange"][data-theme="dark"]',
  );

  assert.match(blueLight, /--ace-bg:\s+#f5f5f2;/);
  assert.match(blueLight, /--ace-accent:\s+#2563eb;/);
  assert.match(blueDark, /--ace-bg:\s+#0f0f0f;/);
  assert.match(blueDark, /--ace-accent:\s+#3b82f6;/);

  for (const body of [orangeLight, orangeDark]) {
    for (const token of [
      '--ace-bg:',
      '--ace-surface:',
      '--ace-surface-alt:',
      '--ace-surface-hi:',
      '--ace-border:',
      '--ace-fg:',
      '--ace-fg-2:',
      '--ace-fg-mute:',
      '--ace-accent:',
      '--ace-accent-bg:',
      '--ace-accent-soft:',
      '--ace-warn:',
      '--ace-warn-bg:',
    ]) {
      assert.ok(body.includes(token), `orange theme is missing ${token}`);
    }
  }
  assert.match(orangeLight, /--ace-accent:\s+#ea5504;/);
  assert.match(orangeLight, /--ace-warn:\s+#b7791f;/);
  assert.match(orangeDark, /--ace-accent:\s+#ff6b1a;/);
  assert.match(orangeDark, /--ace-warn:\s+#fbbf24;/);
});

run('theme previews and primary consumers use CSS-owned theme colors', () => {
  const settings = source('components/SettingsPage.jsx');
  const styles = source('styles/globals.css');
  const guidedTour = source('components/DesktopGuidedTour.jsx');

  assert.match(settings, /`ace-theme-preview-\$\{opt\.key\}`/);
  assert.match(settings, /ace-theme-preview-bg/);
  assert.match(settings, /ace-theme-preview-surface/);
  assert.match(settings, /ace-theme-preview-accent/);
  assert.match(styles, /\.ace-theme-preview-blue\s*\{/);
  assert.match(styles, /\.ace-theme-preview-orange\s*\{/);
  assert.match(styles, /\[data-theme="dark"\] \.ace-theme-preview-blue\s*\{/);
  assert.match(styles, /\[data-theme="dark"\] \.ace-theme-preview-orange\s*\{/);
  assert.match(guidedTour, /primaryColor: 'var\(--ace-accent\)'/);
  assert.doesNotMatch(guidedTour, /primaryColor: '#2563eb'/);
});

run('theme user-facing copy stays generic and localized', () => {
  const settings = source('components/SettingsPage.jsx');
  const colorTheme = source('lib/colorTheme.js');
  const catalog = source('i18n/sourceCatalog.generated.js');

  for (const text of [settings, colorTheme]) {
    assert.doesNotMatch(text, /平安橙|Ping An/i);
  }
  for (const copy of ['蓝色', '橙色', '暗黑模式', '"Blue"', '"Orange"', '"Dark mode"']) {
    assert.ok(catalog.includes(copy), `source catalog is missing ${copy}`);
  }
});
