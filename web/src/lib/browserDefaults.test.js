// Browser default guard tests cover shortcut detection. The listener install
// path is intentionally small, so pure matching is enough for Node-side
// coverage.

import assert from 'node:assert/strict';
import {
  isBlockedBrowserDefaultShortcut,
  isBrowserZoomShortcut,
} from './browserDefaults.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Ctrl+Plus zoom shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: '+', code: 'Equal', ctrlKey: true }), true);
});

run('Ctrl+Minus zoom shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: '-', code: 'Minus', ctrlKey: true }), true);
});

run('Ctrl+0 zoom reset shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: '0', code: 'Digit0', ctrlKey: true }), true);
});

run('Meta+NumpadAdd zoom shortcut is blocked', () => {
  assert.equal(isBrowserZoomShortcut({ key: 'Add', code: 'NumpadAdd', metaKey: true }), true);
});

run('Ctrl+K remains available for app shortcuts', () => {
  assert.equal(isBrowserZoomShortcut({ key: 'k', code: 'KeyK', ctrlKey: true }), false);
});

run('Zoom keys without ctrl/meta are ignored', () => {
  assert.equal(isBrowserZoomShortcut({ key: '+', code: 'Equal' }), false);
});

run('Ctrl+F native find shortcut is blocked', () => {
  assert.equal(isBlockedBrowserDefaultShortcut({ key: 'f', ctrlKey: true }), true);
});

run('F3 native find shortcut is blocked', () => {
  assert.equal(isBlockedBrowserDefaultShortcut({ key: 'F3' }), true);
});
