import assert from 'node:assert/strict';
import { applyLocalePreference, localePreference } from '../i18n/index.js';
import { loadUiLocale, persistUiLocale, syncNativeLocale } from './uiLocale.js';

await applyLocalePreference('zh-CN', { cache: false });

assert.equal(await syncNativeLocale('en-US', {}), null);
const bridgeCalls = [];
assert.deepEqual(await syncNativeLocale('auto', {
  aceDesktop_applyLocale: async (value) => {
    bridgeCalls.push(value);
    return JSON.stringify({ ok: true, preference: value, locale: 'en-US' });
  },
}), { ok: true, preference: 'auto', locale: 'en-US' });
assert.deepEqual(bridgeCalls, ['auto']);

const loaded = await loadUiLocale({
  getUiLocale: async () => ({ locale: 'en-US' }),
}, { scope: {} });
assert.deepEqual(loaded, { preference: 'en-US', locale: 'en-US' });

const savedValues = [];
const saved = await persistUiLocale('auto', {
  setUiLocale: async (value) => {
    savedValues.push(value);
    return { locale: value };
  },
}, {
  scope: {
    aceDesktop_applyLocale: async () => ({ ok: true, locale: 'en-US' }),
  },
});
assert.deepEqual(saved, { preference: 'auto', locale: 'en-US' });
assert.deepEqual(savedValues, ['auto']);

await applyLocalePreference('zh-CN', { cache: false });
await assert.rejects(
  persistUiLocale('en-US', {
    setUiLocale: async () => { throw new Error('disk full'); },
  }, { scope: {} }),
  /disk full/,
);
assert.equal(localePreference(), 'zh-CN');

await applyLocalePreference('zh-CN', { cache: false });
const rollbackWrites = [];
await assert.rejects(
  persistUiLocale('en-US', {
    setUiLocale: async (value) => {
      rollbackWrites.push(value);
      return { locale: value };
    },
  }, {
    scope: { aceDesktop_applyLocale: async () => ({ ok: false, error: 'nope' }) },
  }),
  /nope/,
);
assert.deepEqual(rollbackWrites, ['en-US', 'zh-CN']);
assert.equal(localePreference(), 'zh-CN');
console.log('[pass] GUI locale load, native bridge, persistence, and rollback');
