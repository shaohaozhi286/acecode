import assert from 'node:assert/strict';
import {
  TOPBAR_QUICK_ACTIONS,
  invokeTopBarQuickAction,
  topBarQuickActionNeedsSeparator,
  topBarQuickActionsMenuWidth,
} from './topBarQuickActions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('top-bar quick actions keep the requested labels and order', () => {
  assert.deepEqual(
    TOPBAR_QUICK_ACTIONS.map(({ id, label, group }) => ({ id, label, group })),
    [
      { id: 'new-session', label: '新对话', group: 'navigation' },
      { id: 'new-loop', label: '新建循环', group: 'navigation' },
      { id: 'find-content', label: '查找内容', group: 'navigation' },
      { id: 'settings', label: '设置', group: 'application' },
      { id: 'about', label: '关于 ACECode', group: 'application' },
      { id: 'check-updates', label: '检查更新', group: 'application' },
      { id: 'exit', label: '退出 ACECode', group: 'exit' },
    ],
  );
});

test('top-bar quick actions separate navigation, application, and exit groups', () => {
  assert.deepEqual(
    TOPBAR_QUICK_ACTIONS.map((_, index) => topBarQuickActionNeedsSeparator(index)),
    [false, false, false, true, false, false, true],
  );
});

test('top-bar quick actions invoke only their matching callbacks', () => {
  const calls = [];
  const callbacks = {
    onNewSession: () => calls.push('new-session'),
    onOpenLoop: () => calls.push('new-loop'),
    onOpenSearch: () => calls.push('find-content'),
    onSettings: () => calls.push('settings'),
    onAbout: () => calls.push('about'),
    onCheckUpdates: () => calls.push('check-updates'),
    onExit: () => calls.push('exit'),
  };

  for (const action of TOPBAR_QUICK_ACTIONS) {
    assert.equal(invokeTopBarQuickAction(action.id, callbacks), true);
  }
  assert.deepEqual(calls, [
    'new-session',
    'new-loop',
    'find-content',
    'settings',
    'about',
    'check-updates',
    'exit',
  ]);
});

test('top-bar quick actions ignore unknown actions and absent callbacks', () => {
  assert.equal(invokeTopBarQuickAction('missing', {}), false);
  assert.equal(invokeTopBarQuickAction('settings', {}), false);
});

test('top-bar quick-actions menu follows the project-sidebar width', () => {
  assert.equal(topBarQuickActionsMenuWidth(212), 212);
  assert.equal(topBarQuickActionsMenuWidth(212.6), 213);
});

test('top-bar quick-actions menu uses the default sidebar width for invalid input', () => {
  assert.equal(topBarQuickActionsMenuWidth(undefined), 270);
  assert.equal(topBarQuickActionsMenuWidth(Number.NaN), 270);
  assert.equal(topBarQuickActionsMenuWidth(0), 270);
});
