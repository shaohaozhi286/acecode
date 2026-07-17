import assert from 'node:assert/strict';
import {
  canOpenConversationFind,
  findMatchesInText,
  isComposingInputEvent,
  isFindShortcut,
} from './globalFind.js';

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

run('F3 opens app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'F3' }), true);
});

run('Shift+F3 still routes to app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'F3', shiftKey: true }), true);
});

run('Alt+F3 is left for the host', () => {
  assert.equal(isFindShortcut({ key: 'F3', altKey: true }), false);
});

run('conversation find is enabled only for an unblocked active single-session view', () => {
  const visible = {
    view: 'single',
    activeSessionId: 's1',
  };
  assert.equal(canOpenConversationFind(visible), true);
  assert.equal(canOpenConversationFind({ ...visible, activeSessionId: '' }), false);
  assert.equal(canOpenConversationFind({ ...visible, view: 'grid4' }), false);
  assert.equal(canOpenConversationFind({ ...visible, loop: true }), false);
  for (const blocker of [
    'showSettings',
    'searchOpen',
    'updateDialogOpen',
    'permissionOpen',
    'questionOpen',
    'guidedTourPreparing',
    'guidedTourRun',
  ]) {
    assert.equal(canOpenConversationFind({ ...visible, [blocker]: true }), false, blocker);
  }
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

run('isComposingInputEvent detects native and ref composition state', () => {
  assert.equal(isComposingInputEvent({ isComposing: true }, false), true);
  assert.equal(isComposingInputEvent({ nativeEvent: { isComposing: true } }, false), true);
  assert.equal(isComposingInputEvent({}, true), true);
  assert.equal(isComposingInputEvent({}, false), false);
});
