import assert from 'node:assert/strict';
import {
  GUI_LOCALE_STORAGE_KEY,
  cacheLocalePreference,
  initialLocaleState,
  normalizeLocalePreference,
  resolveLocalePreference,
  systemLocaleToSupported,
} from './locale.js';

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

const writes = [];
assert.equal(cacheLocalePreference('en-US', {
  localStorage: { setItem: (...args) => writes.push(args) },
}), 'en-US');
assert.deepEqual(writes, [[GUI_LOCALE_STORAGE_KEY, 'en-US']]);
console.log('[pass] GUI locale normalization and resolution');
