import assert from 'node:assert/strict';
import {
  filterPinnedSessions,
  normalizePinnedIds,
  pinSessionId,
  pinnedIdSet,
  pinnedSessionsForList,
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

test('filterPinnedSessions 从普通列表中过滤置顶会话', () => {
  const sessions = [{ id: 'a' }, { id: 'b' }, { id: 'c' }];
  const pinned = { w1: ['b'] };
  assert.deepEqual(filterPinnedSessions(sessions, pinned).map((s) => s.id), ['a', 'c']);
  assert.deepEqual(Array.from(pinnedIdSet(pinned)), ['b']);
});
