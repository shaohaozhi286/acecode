import assert from 'node:assert/strict';
import {
  DARK_DESKTOP_ACRYLIC_SETTINGS,
  DEFAULT_DESKTOP_ACRYLIC_SETTINGS,
  DESKTOP_ACRYLIC_SETTINGS_STORAGE_KEY,
  LIGHT_DESKTOP_ACRYLIC_SETTINGS,
  applyDesktopAcrylicCssVariables,
  applyNativeDesktopAcrylicSettings,
  defaultDesktopAcrylicSettingsForTheme,
  hexToRgb,
  loadInitialDesktopAcrylicSettings,
  loadStoredDesktopAcrylicSettings,
  normalizeDesktopAcrylicSettings,
  normalizeNativeAcrylicResponse,
  saveDesktopAcrylicSettings,
  settingsToNativeAcrylicPayload,
} from './desktopAcrylicSettings.js';

function run(name, fn) {
  try {
    const ret = fn();
    if (ret && typeof ret.then === 'function') {
      return ret.then(
        () => console.log(`[pass] ${name}`),
        (err) => { console.error(`[fail] ${name}`); throw err; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function makeStorage(initial = {}) {
  const store = new Map(Object.entries(initial));
  return {
    getItem(key) {
      return store.has(key) ? store.get(key) : null;
    },
    setItem(key, value) {
      store.set(key, String(value));
    },
    store,
  };
}

function makeRoot() {
  const props = new Map();
  return {
    style: {
      setProperty(key, value) {
        props.set(key, value);
      },
    },
    props,
  };
}

run('default acrylic settings are theme-specific', () => {
  assert.equal(DEFAULT_DESKTOP_ACRYLIC_SETTINGS, LIGHT_DESKTOP_ACRYLIC_SETTINGS);
  assert.equal(defaultDesktopAcrylicSettingsForTheme('light').tintColor, '#f2f3f8');
  assert.equal(defaultDesktopAcrylicSettingsForTheme('light').tintOpacity, 1);
  assert.equal(defaultDesktopAcrylicSettingsForTheme('dark').tintColor, '#1c1f21');
  assert.equal(defaultDesktopAcrylicSettingsForTheme('dark').sidebarTintOpacity, 0.42);
});

run('normalizeDesktopAcrylicSettings clamps opacity and normalizes color', () => {
  const settings = normalizeDesktopAcrylicSettings({
    tintColor: 'AABBCC',
    tintOpacity: 1.4,
    sidebarTintOpacity: -1,
    hoverOpacity: 0.3333,
    activeOpacity: 'bad',
  });
  assert.equal(settings.tintColor, '#aabbcc');
  assert.equal(settings.tintOpacity, 1);
  assert.equal(settings.sidebarTintOpacity, 0);
  assert.equal(settings.hoverOpacity, 0.3333);
  assert.equal(settings.activeOpacity, DEFAULT_DESKTOP_ACRYLIC_SETTINGS.activeOpacity);
});

run('normalizeDesktopAcrylicSettings accepts native tintAlpha', () => {
  const settings = normalizeDesktopAcrylicSettings({ tintAlpha: 64 });
  assert.equal(settings.tintOpacity, 64 / 255);
});

run('hexToRgb parses normalized color', () => {
  assert.deepEqual(hexToRgb('#0a1b2c'), { red: 10, green: 27, blue: 44 });
});

run('storage helpers persist normalized settings', () => {
  const storage = makeStorage();
  const saved = saveDesktopAcrylicSettings({ tintColor: '#ABCDEF', tintOpacity: 0.25 }, storage, 'dark');
  assert.equal(saved.tintColor, '#abcdef');
  const stored = JSON.parse(storage.store.get(DESKTOP_ACRYLIC_SETTINGS_STORAGE_KEY));
  assert.equal(stored.dark.tintOpacity, 0.25);
  assert.equal(loadStoredDesktopAcrylicSettings(storage, 'dark').tintColor, '#abcdef');
  assert.equal(loadStoredDesktopAcrylicSettings(storage, 'light'), null);
});

run('applyDesktopAcrylicCssVariables writes desktop-only CSS variables', () => {
  const root = makeRoot();
  applyDesktopAcrylicCssVariables(root, {
    tintColor: '#336699',
    tintOpacity: 0.5,
    sidebarTintOpacity: 0.125,
    hoverOpacity: 0.5,
    activeOpacity: 0.2,
  });
  assert.equal(root.props.get('--ace-sidebar-acrylic-tint-rgb'), '51, 102, 153');
  assert.equal(root.props.get('--ace-sidebar-acrylic-tint-alpha'), '0.125');
  assert.equal(root.props.get('--ace-sidebar-acrylic-hover-alpha'), '0.5');
  assert.equal(root.props.get('--ace-sidebar-acrylic-active-alpha'), '0.2');
});

run('settingsToNativeAcrylicPayload maps opacity to alpha byte', () => {
  assert.deepEqual(settingsToNativeAcrylicPayload({
    tintColor: '#123456',
    tintOpacity: 0.5,
  }), {
    tintColor: '#123456',
    tintOpacity: 0.5,
    tintAlpha: 128,
  });
});

run('normalizeNativeAcrylicResponse parses bridge JSON string', () => {
  const settings = normalizeNativeAcrylicResponse(JSON.stringify({
    ok: true,
    tintColor: '#010203',
    tintAlpha: 200,
  }));
  assert.equal(settings.tintColor, '#010203');
  assert.equal(settings.tintOpacity, 200 / 255);
});

await run('loadInitialDesktopAcrylicSettings prefers stored values over native bridge', async () => {
  const storage = makeStorage({
    [DESKTOP_ACRYLIC_SETTINGS_STORAGE_KEY]: JSON.stringify({ dark: { tintColor: '#445566' } }),
  });
  const settings = await loadInitialDesktopAcrylicSettings({
    localStorage: storage,
    aceDesktop_getSidebarAcrylicSettings: async () => JSON.stringify({ ok: true, tintColor: '#112233' }),
  }, storage, 'dark');
  assert.equal(settings.tintColor, '#445566');
});

await run('loadInitialDesktopAcrylicSettings uses theme default without stored values', async () => {
  const storage = makeStorage();
  const settings = await loadInitialDesktopAcrylicSettings({
    localStorage: storage,
    aceDesktop_getSidebarAcrylicSettings: async () => JSON.stringify({ ok: true, tintColor: '#112233' }),
  }, storage, 'dark');
  assert.equal(settings.tintColor, DARK_DESKTOP_ACRYLIC_SETTINGS.tintColor);
  assert.equal(settings.tintOpacity, 1);
  assert.equal(settings.sidebarTintOpacity, 0.42);
});

await run('applyNativeDesktopAcrylicSettings sends native payload', async () => {
  let captured = null;
  const win = {
    aceDesktop_setSidebarAcrylicSettings: async (payload) => {
      captured = payload;
      return JSON.stringify({ ok: true, ...payload });
    },
  };
  const result = await applyNativeDesktopAcrylicSettings({ tintColor: '#abcdef', tintOpacity: 0.25 }, win);
  assert.deepEqual(captured, { tintColor: '#abcdef', tintOpacity: 0.25, tintAlpha: 64 });
  assert.equal(result.tintOpacity, 64 / 255);
});
