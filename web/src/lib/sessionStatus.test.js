// Sidebar session attention 状态合并测试。

import assert from 'node:assert/strict';
import {
  applyStatusSnapshot,
  applyStatusUpdate,
  mergeSessionStatus,
  sessionAttentionState,
  workspaceHasUnread,
} from './sessionStatus.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('缺失状态字段默认视为已读', () => {
  assert.equal(sessionAttentionState({ id: 's1' }), 'read');
});

run('running/busy 优先显示进行中', () => {
  assert.equal(sessionAttentionState({ id: 's1', attention_state: 'unread', busy: true }), 'in_progress');
  assert.equal(sessionAttentionState({ id: 's1', status: 'running' }), 'in_progress');
});

run('WebSocket snapshot 合并到状态 map', () => {
  const map = applyStatusSnapshot(new Map(), {
    workspace_hash: 'w1',
    sessions: [
      { session_id: 's1', workspace_hash: 'w1', state: 'unread', cursor: 4 },
      { session_id: 's2', workspace_hash: 'w1', state: 'read', cursor: 1 },
    ],
  });
  assert.equal(map.get('s1').attention_state, 'unread');
  assert.equal(map.get('s1').status_cursor, 4);
  assert.equal(map.get('s2').attention_state, 'read');
});

run('增量状态覆盖 HTTP session 字段', () => {
  const map = applyStatusUpdate(new Map(), {
    session_id: 's1',
    workspace_hash: 'w1',
    state: 'unread',
    cursor: 9,
    timestamp_ms: 100,
  });
  const merged = mergeSessionStatus({ id: 's1', status: 'idle', attention_state: 'read' }, map);
  assert.equal(merged.attention_state, 'unread');
  assert.equal(merged.status_cursor, 9);
});

run('WS idle 状态覆盖 HTTP running 旧值', () => {
  const map = applyStatusUpdate(new Map(), {
    session_id: 's1',
    state: 'read',
    busy: false,
    timestamp_ms: 100,
  });
  const merged = mergeSessionStatus({ id: 's1', status: 'running', busy: true }, map);
  assert.equal(merged.attention_state, 'read');
  assert.equal(merged.status, 'idle');
});

run('旧增量不会覆盖较新的状态', () => {
  let map = applyStatusUpdate(new Map(), { session_id: 's1', state: 'unread', timestamp_ms: 200 });
  map = applyStatusUpdate(map, { session_id: 's1', state: 'read', timestamp_ms: 100 });
  assert.equal(map.get('s1').attention_state, 'unread');
});

run('项目未读只由 child unread 聚合', () => {
  assert.equal(workspaceHasUnread([{ id: 's1', attention_state: 'read' }, { id: 's2', attention_state: 'in_progress' }]), false);
  assert.equal(workspaceHasUnread([{ id: 's1', attention_state: 'unread' }]), true);
});
