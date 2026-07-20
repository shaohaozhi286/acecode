import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  bindDesktopComposerAutoFocus,
  requestDesktopWindowFocus,
  restoreComposerTextareaCaret,
  shouldAutoFocusDesktopComposer,
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

function eventTarget() {
  const listeners = new Map();
  return {
    addEventListener(type, listener) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(listener);
    },
    removeEventListener(type, listener) {
      listeners.get(type)?.delete(listener);
    },
    dispatch(type, event = {}) {
      for (const listener of listeners.get(type) || []) listener({ ...event, type });
    },
    listenerCount(type) {
      return listeners.get(type)?.size || 0;
    },
  };
}

function terminalFocusTarget({ collapsed = false, connected = true } = {}) {
  const region = {
    isConnected: connected,
    getAttribute(name) {
      return name === 'data-collapsed' ? String(collapsed) : null;
    },
  };
  return Object.assign(element('terminal-textarea'), {
    closest(selector) {
      return selector === '[data-ace-focus-region="terminal"]' ? region : null;
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

run('desktop composer auto-focus is enabled only for an unobstructed visible shell chat', () => {
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'shell',
    chatVisible: true,
    blockingSurfaceOpen: false,
  }), true);
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'browser',
    chatVisible: true,
    blockingSurfaceOpen: false,
  }), false);
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'shell',
    chatVisible: false,
    blockingSurfaceOpen: false,
  }), false);
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'shell',
    chatVisible: true,
    blockingSurfaceOpen: true,
  }), false);
});

run('integrated terminal advertises the desktop focus region contract', () => {
  const source = readFileSync(new URL('../components/ConsoleDock.jsx', import.meta.url), 'utf8');
  assert.match(
    source,
    /className="ace-console-dock"\s+data-ace-focus-region="terminal"/,
  );
});

run('desktop composer auto-focus follows focus visibility events and cleans up', () => {
  const win = eventTarget();
  const documentRef = Object.assign(eventTarget(), {
    visibilityState: 'visible',
    querySelector: () => null,
  });
  let focusCalls = 0;
  const cleanup = bindDesktopComposerAutoFocus({
    enabled: true,
    onFocus: () => { focusCalls += 1; },
    win,
    documentRef,
  });

  assert.equal(win.listenerCount('focus'), 1);
  assert.equal(win.listenerCount('pageshow'), 1);
  assert.equal(win.listenerCount('acecode:desktop-window-focus'), 1);
  assert.equal(documentRef.listenerCount('focusin'), 1);
  assert.equal(documentRef.listenerCount('visibilitychange'), 1);
  win.dispatch('focus');
  win.dispatch('pageshow');
  win.dispatch('acecode:desktop-window-focus');
  assert.equal(focusCalls, 3);

  documentRef.visibilityState = 'hidden';
  win.dispatch('focus');
  documentRef.dispatch('visibilitychange');
  assert.equal(focusCalls, 3);

  documentRef.visibilityState = 'visible';
  documentRef.dispatch('visibilitychange');
  assert.equal(focusCalls, 4);

  cleanup();
  win.dispatch('focus');
  win.dispatch('acecode:desktop-window-focus');
  assert.equal(focusCalls, 4);
  assert.equal(win.listenerCount('focus'), 0);
  assert.equal(win.listenerCount('acecode:desktop-window-focus'), 0);
  assert.equal(documentRef.listenerCount('focusin'), 0);
  assert.equal(documentRef.listenerCount('visibilitychange'), 0);
});

run('desktop composer auto-focus preserves expanded terminal ownership on reactivation', () => {
  const win = eventTarget();
  const body = element('body');
  const html = element('html');
  const documentRef = Object.assign(eventTarget(), {
    activeElement: body,
    body,
    documentElement: html,
    visibilityState: 'visible',
    querySelector: () => null,
  });
  const terminalInput = terminalFocusTarget();
  let focusCalls = 0;
  const cleanup = bindDesktopComposerAutoFocus({
    enabled: true,
    onFocus: () => { focusCalls += 1; },
    win,
    documentRef,
  });

  documentRef.activeElement = terminalInput;
  documentRef.dispatch('focusin', { target: terminalInput });
  documentRef.activeElement = body;
  win.dispatch('focus');
  win.dispatch('acecode:desktop-window-focus');
  documentRef.dispatch('visibilitychange');
  assert.equal(focusCalls, 0);
  cleanup();
});

run('desktop composer auto-focus resumes after focus leaves or closes the terminal', () => {
  const win = eventTarget();
  const body = element('body');
  const html = element('html');
  const documentRef = Object.assign(eventTarget(), {
    activeElement: body,
    body,
    documentElement: html,
    visibilityState: 'visible',
    querySelector: () => null,
  });
  let focusCalls = 0;
  const cleanup = bindDesktopComposerAutoFocus({
    enabled: true,
    onFocus: () => { focusCalls += 1; },
    win,
    documentRef,
  });

  const terminalInput = terminalFocusTarget();
  const composerInput = textarea('draft');
  documentRef.dispatch('focusin', { target: terminalInput });
  documentRef.dispatch('focusin', { target: composerInput });
  win.dispatch('focus');
  assert.equal(focusCalls, 1);

  const collapsedTerminalInput = terminalFocusTarget({ collapsed: true });
  documentRef.dispatch('focusin', { target: collapsedTerminalInput });
  win.dispatch('focus');
  assert.equal(focusCalls, 2);
  cleanup();
});

run('desktop composer auto-focus does not steal focus through a modal surface', () => {
  const win = eventTarget();
  const documentRef = Object.assign(eventTarget(), {
    visibilityState: 'visible',
    querySelector: () => ({ role: 'dialog' }),
  });
  let focusCalls = 0;
  const cleanup = bindDesktopComposerAutoFocus({
    enabled: true,
    onFocus: () => { focusCalls += 1; },
    win,
    documentRef,
  });

  win.dispatch('focus');
  assert.equal(focusCalls, 0);
  cleanup();
});

run('desktop composer auto-focus stays inert when disabled', () => {
  const win = eventTarget();
  const documentRef = Object.assign(eventTarget(), {
    visibilityState: 'visible',
    querySelector: () => null,
  });
  const cleanup = bindDesktopComposerAutoFocus({
    enabled: false,
    onFocus: () => { throw new Error('must not focus'); },
    win,
    documentRef,
  });

  assert.equal(win.listenerCount('focus'), 0);
  cleanup();
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
