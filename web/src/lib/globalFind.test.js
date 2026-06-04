import assert from 'node:assert/strict';
import { findMatchesInText, isFindShortcut } from './globalFind.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Ctrl+F opens app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'f', ctrlKey: true }), true);
});

run('Cmd+F opens app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'F', metaKey: true }), true);
});

run('Alt+F is not treated as find shortcut', () => {
  assert.equal(isFindShortcut({ key: 'f', altKey: true }), false);
});

run('findMatchesInText finds case-insensitive non-overlapping matches', () => {
  assert.deepEqual(findMatchesInText('Green green GREEN', 'green'), [
    { start: 0, end: 5 },
    { start: 6, end: 11 },
    { start: 12, end: 17 },
  ]);
});

run('findMatchesInText handles CJK text', () => {
  assert.deepEqual(findMatchesInText('绿色和深绿色', '绿色'), [
    { start: 0, end: 2 },
    { start: 4, end: 6 },
  ]);
});

run('findMatchesInText ignores empty query', () => {
  assert.deepEqual(findMatchesInText('abc', ''), []);
});
