import assert from 'node:assert/strict';
import {
  COLOR_THEME_STORAGE_KEY,
  COLOR_THEME_VALUES,
  DEFAULT_COLOR_THEME,
  effectiveColorTheme,
  isValidColorTheme,
} from './colorTheme.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('color theme preference keeps a separate stable storage key', () => {
  assert.equal(COLOR_THEME_STORAGE_KEY, 'ace.colorTheme');
});

run('color theme preference defaults to blue', () => {
  assert.equal(DEFAULT_COLOR_THEME, 'blue');
  assert.equal(effectiveColorTheme(undefined), 'blue');
  assert.equal(effectiveColorTheme(null), 'blue');
});

run('color theme preference accepts only blue and orange', () => {
  assert.deepEqual(COLOR_THEME_VALUES, ['blue', 'orange']);
  assert.equal(isValidColorTheme('blue'), true);
  assert.equal(isValidColorTheme('orange'), true);
});

run('color theme preference rejects and normalizes invalid values', () => {
  for (const value of ['light', 'dark', 'red', '', 1, {}, []]) {
    assert.equal(isValidColorTheme(value), false);
    assert.equal(effectiveColorTheme(value), 'blue');
  }
});

run('effective color theme preserves valid stored values', () => {
  assert.equal(effectiveColorTheme('blue'), 'blue');
  assert.equal(effectiveColorTheme('orange'), 'orange');
});
