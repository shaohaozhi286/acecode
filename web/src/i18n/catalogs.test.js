import assert from 'node:assert/strict';
import { translationCatalogs } from './catalogs.js';

function leafEntries(value, prefix = '') {
  return Object.entries(value).flatMap(([key, child]) => {
    const path = prefix ? `${prefix}.${key}` : key;
    if (child && typeof child === 'object' && !Array.isArray(child)) {
      return leafEntries(child, path);
    }
    return [[path, child]];
  });
}

function placeholders(value) {
  return [...String(value).matchAll(/{{\s*([^},\s]+)[^}]*}}/g)]
    .map((match) => match[1])
    .sort();
}

const zhEntries = new Map(leafEntries(translationCatalogs['zh-CN']));
const enEntries = new Map(leafEntries(translationCatalogs['en-US']));

assert.deepEqual([...enEntries.keys()].sort(), [...zhEntries.keys()].sort());
for (const [key, zhValue] of zhEntries) {
  assert.equal(typeof zhValue, 'string', `${key} must be a string in zh-CN`);
  assert.equal(typeof enEntries.get(key), 'string', `${key} must be a string in en-US`);
  assert.deepEqual(
    placeholders(enEntries.get(key)),
    placeholders(zhValue),
    `${key} interpolation placeholders must match`,
  );
}
console.log('[pass] i18n catalogs keep key and interpolation parity');
