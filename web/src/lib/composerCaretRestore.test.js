import assert from 'node:assert/strict';
import {
  requestDesktopWindowFocus,
  restoreComposerTextareaCaret,
  shouldRestoreComposerTextareaFocus,
} from './composerCaretRestore.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function element(name) {
  const children = new Set();
  return {
    name,
    appendChild(child) {
      children.add(child);
      return child;
    },
    contains(target) {
      return target === this || children.has(target);
    },
  };
}

function textarea(value = '') {
  const el = element('textarea');
  return Object.assign(el, {
    value,
    selectionStart: 0,
    selectionEnd: 0,
    selectionDirection: 'none',
    focusCalls: 0,
    selectionCalls: [],
    focus() {
      this.focusCalls += 1;
    },
    setSelectionRange(start, end, direction) {
      this.selectionStart = start;
      this.selectionEnd = end;
      this.selectionDirection = direction;
      this.selectionCalls.push({ start, end, direction });
    },
  });
}

run('composer caret restore is allowed when focus fell back to body', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  input.selectionStart = 5;
  input.selectionEnd = 5;

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 2, end: 2, direction: 'none' },
    documentRef: { activeElement: body, body, documentElement: html },
  });

  assert.equal(restored, true);
  assert.equal(input.focusCalls, 1);
  assert.deepEqual(input.selectionCalls.at(-1), { start: 2, end: 2, direction: 'none' });
});

run('composer caret restore is skipped for focus outside the composer', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  const externalInput = textarea('outside');

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 2, end: 2, direction: 'none' },
    documentRef: { activeElement: externalInput, body, documentElement: html },
  });

  assert.equal(restored, false);
  assert.equal(input.focusCalls, 0);
  assert.equal(input.selectionCalls.length, 0);
});

run('composer caret restore can intentionally focus from an external drop source', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  const externalButton = element('button');

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 3, end: 3, direction: 'none' },
    documentRef: { activeElement: externalButton, body, documentElement: html },
    allowExternalFocus: true,
  });

  assert.equal(restored, true);
  assert.equal(input.focusCalls, 1);
  assert.deepEqual(input.selectionCalls.at(-1), { start: 3, end: 3, direction: 'none' });
});

run('composer caret restore uses live selection when textarea is already active', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  input.selectionStart = 4;
  input.selectionEnd = 4;

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 1, end: 1, direction: 'none' },
    documentRef: { activeElement: input, body, documentElement: html },
  });

  assert.equal(restored, true);
  assert.deepEqual(input.selectionCalls.at(-1), { start: 4, end: 4, direction: 'none' });
});

run('composer focus restore allows active descendants inside composer root', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  const addButton = root.appendChild(element('button'));

  assert.equal(shouldRestoreComposerTextareaFocus({
    activeElement: addButton,
    rootElement: root,
    textareaElement: input,
    bodyElement: body,
    documentElement: html,
  }), true);
});

run('desktop window focus request calls optional bridge', () => {
  let calls = 0;
  const ok = requestDesktopWindowFocus({
    aceDesktop_focusWindow: () => {
      calls += 1;
      return Promise.resolve('{"ok":true}');
    },
  });

  assert.equal(ok, true);
  assert.equal(calls, 1);
});

run('desktop window focus request is a no-op without bridge', () => {
  assert.equal(requestDesktopWindowFocus({}), false);
  assert.equal(requestDesktopWindowFocus(null), false);
});

run('desktop window focus request swallows bridge throw', () => {
  const ok = requestDesktopWindowFocus({
    aceDesktop_focusWindow: () => {
      throw new Error('native unavailable');
    },
  });

  assert.equal(ok, false);
});
