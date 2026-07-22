import assert from 'node:assert/strict';
import {
  GUI_LOCALE_RELOAD_STORAGE_KEY,
  GUI_LOCALE_STORAGE_KEY,
  cacheLocalePreference,
  cacheLocaleReloadOverride,
  initialLocaleState,
  normalizeLocalePreference,
  resolveLocalePreference,
  systemLocaleToSupported,
} from './locale.js';

function memoryStorage(initial = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem: (key) => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, String(value)),
    removeItem: (key) => values.delete(key),
  };
}

assert.equal(normalizeLocalePreference('auto'), 'auto');
assert.equal(normalizeLocalePreference('en-US'), 'en-US');
assert.equal(normalizeLocalePreference('bogus'), 'zh-CN');
assert.equal(systemLocaleToSupported('zh-TW'), 'zh-CN');
assert.equal(systemLocaleToSupported('en-GB'), 'en-US');
assert.equal(resolveLocalePreference('auto', 'zh-Hant-TW'), 'zh-CN');
assert.equal(resolveLocalePreference('auto', 'de-DE'), 'en-US');
assert.deepEqual(initialLocaleState({ navigator: { language: 'en-US' } }), {
  preference: 'zh-CN',
  locale: 'zh-CN',
});
assert.deepEqual(initialLocaleState({
  __ACECODE_LOCALE_PREFERENCE__: 'auto',
  __ACECODE_LOCALE__: 'en-US',
}), { preference: 'auto', locale: 'en-US' });

const reloadStorage = memoryStorage();
const reloadScope = {
  __ACECODE_LOCALE_PREFERENCE__: 'zh-CN',
  __ACECODE_LOCALE__: 'zh-CN',
  sessionStorage: reloadStorage,
};
assert.deepEqual(
  cacheLocaleReloadOverride('en-US', 'en-US', reloadScope),
  { preference: 'en-US', locale: 'en-US' },
);
assert.deepEqual(initialLocaleState(reloadScope), {
  preference: 'en-US',
  locale: 'en-US',
});
assert.equal(reloadStorage.getItem(GUI_LOCALE_RELOAD_STORAGE_KEY), null);
assert.deepEqual(initialLocaleState(reloadScope), {
  preference: 'zh-CN',
  locale: 'zh-CN',
});

const writes = [];
assert.equal(cacheLocalePreference('en-US', {
  localStorage: { setItem: (...args) => writes.push(args) },
}), 'en-US');
assert.deepEqual(writes, [[GUI_LOCALE_STORAGE_KEY, 'en-US']]);
console.log('[pass] GUI locale normalization and resolution');
