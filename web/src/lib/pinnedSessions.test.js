import assert from 'node:assert/strict';
import {
  filterPinnedSessions,
  normalizePinnedIds,
  normalizePinnedOrderItems,
  pinSessionId,
  pinPinnedOrderItem,
  pinnedIdSet,
  pinnedOrderItemsForSessions,
  pinnedSessionsForList,
  reorderPinnedOrderItems,
  reorderPinnedSessionId,
  unpinPinnedOrderItem,
  unpinSessionId,
} from './pinnedSessions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('pinned ids 去空并保持首次出现顺序', () => {
  assert.deepEqual(normalizePinnedIds(['a', '', 'b', 'a', null, 'c']), ['a', 'b', 'c']);
});

test('pinSessionId 将会话移动到置顶列表顶部', () => {
  assert.deepEqual(pinSessionId(['a', 'b', 'c'], 'b'), ['b', 'a', 'c']);
  assert.deepEqual(pinSessionId(['a', 'b'], 'c'), ['c', 'a', 'b']);
});

test('unpinSessionId 从置顶列表移除会话', () => {
  assert.deepEqual(unpinSessionId(['a', 'b', 'c'], 'b'), ['a', 'c']);
});

test('reorderPinnedSessionId 在目标前后移动会话', () => {
  assert.deepEqual(reorderPinnedSessionId(['a', 'b', 'c'], 'c', 'a', 'before'), ['c', 'a', 'b']);
  assert.deepEqual(reorderPinnedSessionId(['a', 'b', 'c'], 'a', 'c', 'after'), ['b', 'c', 'a']);
});

test('reorderPinnedSessionId 对无效拖拽保持原顺序', () => {
  assert.deepEqual(reorderPinnedSessionId(['a', 'b', 'c'], 'missing', 'a'), ['a', 'b', 'c']);
  assert.deepEqual(reorderPinnedSessionId(['a', 'b', 'c'], 'a', 'missing'), ['a', 'b', 'c']);
  assert.deepEqual(reorderPinnedSessionId(['a', 'b', 'c'], 'a', 'a'), ['a', 'b', 'c']);
});

test('pinnedSessionsForList 按 workspace pin 顺序投影可见会话', () => {
  const sessions = [
    { id: 'a', workspace_hash: 'w1', title: 'A' },
    { id: 'b', workspace_hash: 'w1', title: 'B' },
    { id: 'c', workspace_hash: 'w2', title: 'C' },
  ];
  const pinned = new Map([
    ['w1', ['b', 'a']],
    ['w2', ['missing', 'c']],
  ]);
  assert.deepEqual(pinnedSessionsForList(sessions, pinned).map((s) => s.id), ['b', 'a', 'c']);
});

test('pinnedSessionsForList 按全局视觉顺序跨 workspace 排列', () => {
  const sessions = [
    { id: 'a', workspace_hash: 'w1', title: 'A' },
    { id: 'b', workspace_hash: 'w1', title: 'B' },
    { id: 'x', workspace_hash: 'w2', title: 'X' },
  ];
  const pinned = new Map([
    ['w1', ['a', 'b']],
    ['w2', ['x']],
  ]);
  const order = [
    { workspace_hash: 'w1', session_id: 'b' },
    { workspace_hash: 'w2', session_id: 'x' },
    { workspace_hash: 'w1', session_id: 'a' },
  ];
  assert.deepEqual(pinnedSessionsForList(sessions, pinned, order).map((s) => `${s.workspace_hash}:${s.id}`), [
    'w1:b',
    'w2:x',
    'w1:a',
  ]);
});

test('pinnedSessionsForList 全局顺序缺项时按旧顺序补齐', () => {
  const sessions = [
    { id: 'a', workspace_hash: 'w1' },
    { id: 'b', workspace_hash: 'w1' },
    { id: 'x', workspace_hash: 'w2' },
  ];
  const pinned = { w1: ['a', 'b'], w2: ['x'] };
  const order = [{ workspace_hash: 'w2', session_id: 'x' }];
  assert.deepEqual(pinnedSessionsForList(sessions, pinned, order).map((s) => s.id), ['x', 'a', 'b']);
});

test('pinned order helpers normalize, reorder, pin, and unpin visual rows', () => {
  const order = normalizePinnedOrderItems([
    { workspace_hash: 'w1', session_id: 'a' },
    { workspace_hash: 'w1', session_id: 'a' },
    { workspace_hash: '', session_id: 'missing' },
    { workspaceHash: 'w2', sessionId: 'x' },
  ]);
  assert.deepEqual(order, [
    { workspace_hash: 'w1', session_id: 'a' },
    { workspace_hash: 'w2', session_id: 'x' },
  ]);

  const moved = reorderPinnedOrderItems([
    { workspace_hash: 'w1', session_id: 'a' },
    { workspace_hash: 'w1', session_id: 'b' },
    { workspace_hash: 'w2', session_id: 'x' },
  ], { workspace_hash: 'w1', session_id: 'a' }, { workspace_hash: 'w2', session_id: 'x' }, 'after');
  assert.deepEqual(moved.map((item) => `${item.workspace_hash}:${item.session_id}`), ['w1:b', 'w2:x', 'w1:a']);

  const pinnedTop = pinPinnedOrderItem(moved, { workspace_hash: 'w3', session_id: 'z' });
  assert.deepEqual(pinnedTop.map((item) => `${item.workspace_hash}:${item.session_id}`), ['w3:z', 'w1:b', 'w2:x', 'w1:a']);
  assert.deepEqual(unpinPinnedOrderItem(pinnedTop, { workspace_hash: 'w2', session_id: 'x' }).map((item) => `${item.workspace_hash}:${item.session_id}`), ['w3:z', 'w1:b', 'w1:a']);
});

test('pinnedOrderItemsForSessions extracts visible pinned order keys', () => {
  assert.deepEqual(pinnedOrderItemsForSessions([
    { id: 'a', workspace_hash: 'w1' },
    { session_id: 'x', workspaceHash: 'w2' },
  ]), [
    { workspace_hash: 'w1', session_id: 'a' },
    { workspace_hash: 'w2', session_id: 'x' },
  ]);
});

test('filterPinnedSessions 从普通列表中过滤置顶会话', () => {
  const sessions = [{ id: 'a' }, { id: 'b' }, { id: 'c' }];
  const pinned = { w1: ['b'] };
  assert.deepEqual(filterPinnedSessions(sessions, pinned).map((s) => s.id), ['a', 'c']);
  assert.deepEqual(Array.from(pinnedIdSet(pinned)), ['b']);
});
