import assert from 'node:assert/strict';
import { TOPBAR_QUICK_ACTIONS, invokeTopBarQuickAction } from './topBarQuickActions.js';

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
    TOPBAR_QUICK_ACTIONS.map(({ id, label }) => ({ id, label })),
    [
      { id: 'new-session', label: '新对话' },
      { id: 'new-loop', label: '新建循环' },
      { id: 'find-content', label: '查找内容' },
      { id: 'settings', label: '设置' },
    ],
  );
});

test('top-bar quick actions invoke only their matching callbacks', () => {
  const calls = [];
  const callbacks = {
    onNewSession: () => calls.push('new-session'),
    onOpenLoop: () => calls.push('new-loop'),
    onOpenSearch: () => calls.push('find-content'),
    onSettings: () => calls.push('settings'),
  };

  for (const action of TOPBAR_QUICK_ACTIONS) {
    assert.equal(invokeTopBarQuickAction(action.id, callbacks), true);
  }
  assert.deepEqual(calls, ['new-session', 'new-loop', 'find-content', 'settings']);
});

test('top-bar quick actions ignore unknown actions and absent callbacks', () => {
  assert.equal(invokeTopBarQuickAction('missing', {}), false);
  assert.equal(invokeTopBarQuickAction('settings', {}), false);
});
