import assert from 'node:assert/strict';
import {
  dismissChangeDockSignature,
  dismissedDockSignatureFor,
  dockDismissalKey,
  isTodoDockSuppressed,
  todoDockSignature,
  validateDockDismissals,
} from './changeDockDismissal.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('dockDismissalKey 按 workspace 和 session 隔离', () => {
  assert.equal(dockDismissalKey({ workspaceHash: 'w1' }, 's1'), 'w1:s1');
  assert.equal(dockDismissalKey({ workspaceHash: '' }, 's1'), 's1');
  assert.equal(dockDismissalKey({ sessionId: 's2' }), 's2');
  assert.equal(dockDismissalKey({}, ''), '');
});

run('dock dismissal 只隐藏同一 session 的同一签名', () => {
  const first = dockDismissalKey({ workspaceHash: 'w' }, 's1');
  const second = dockDismissalKey({ workspaceHash: 'w' }, 's2');
  const state = dismissChangeDockSignature({}, first, 'sig-a');
  assert.equal(dismissedDockSignatureFor(state, first), 'sig-a');
  assert.equal(dismissedDockSignatureFor(state, second), '');

  const replaced = dismissChangeDockSignature(state, first, 'sig-b');
  assert.equal(dismissedDockSignatureFor(replaced, first), 'sig-b');
});

run('dock dismissal state 可 JSON 持久化并恢复', () => {
  const key = dockDismissalKey({ workspaceHash: 'w' }, 's1');
  const state = dismissChangeDockSignature({}, key, 'sig-a');
  const restored = JSON.parse(JSON.stringify(state));
  assert.equal(validateDockDismissals(restored), true);
  assert.equal(dismissedDockSignatureFor(restored, key), 'sig-a');
});

run('validateDockDismissals 拒绝无效结构', () => {
  assert.equal(validateDockDismissals({ a: 'sig' }), true);
  assert.equal(validateDockDismissals(null), false);
  assert.equal(validateDockDismissals([]), false);
  assert.equal(validateDockDismissals({ a: '' }), false);
  assert.equal(validateDockDismissals({ a: 1 }), false);
});

run('todoDockSignature 对相同 todo 快照稳定,内容变化即变', () => {
  // 场景:用户提交下一轮对话时记录 todo 快照签名;本轮 agent 尚未动 todo
  // (签名不变)dock 保持收起,agent 更新任一 todo 状态后签名变化 → 重现。
  const todos = [{ id: '1', content: 'a', status: 'pending' }];
  const summary = { completed: 0, total: 1 };
  assert.equal(todoDockSignature(todos, summary), todoDockSignature([...todos], { ...summary }));
  assert.notEqual(
    todoDockSignature(todos, summary),
    todoDockSignature([{ ...todos[0], status: 'completed' }], summary),
  );
  // 空/非法输入不抛异常(会话切换瞬时帧 todos 可能是 null)。
  assert.equal(todoDockSignature(null, null), todoDockSignature([], null));
});

run('isTodoDockSuppressed 只在同会话同签名时抑制', () => {
  // 场景:提交时记 {sessionKey, signature};期望:同会话同签名 → 抑制;
  // 切换会话(sessionKey 不同)或 todo 更新(签名不同)→ 不抑制;
  // 初始 state 为 null(从未提交过)→ 不抑制。
  const sig = todoDockSignature([{ id: '1', content: 'a', status: 'pending' }], null);
  const state = { sessionKey: 's1', signature: sig };
  assert.equal(isTodoDockSuppressed(state, 's1', sig), true);
  assert.equal(isTodoDockSuppressed(state, 's2', sig), false);
  assert.equal(isTodoDockSuppressed(state, 's1', 'other'), false);
  assert.equal(isTodoDockSuppressed(null, 's1', sig), false);
  assert.equal(isTodoDockSuppressed(state, '', sig), false);
});