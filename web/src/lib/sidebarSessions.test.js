import assert from 'node:assert/strict';
import {
  SIDEBAR_SESSION_COLLAPSE_LIMIT,
  sidebarSessionProjection,
  sortSidebarSessionsNewestFirst,
  upsertSidebarSession,
} from './sidebarSessions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('five or fewer sidebar sessions are not collapsible', () => {
  const sessions = Array.from({ length: SIDEBAR_SESSION_COLLAPSE_LIMIT }, (_, i) => ({ id: String(i) }));
  const result = sidebarSessionProjection(sessions, false);
  assert.equal(result.collapsible, false);
  assert.equal(result.action, '');
  assert.deepEqual(result.visibleSessions.map((s) => s.id), ['0', '1', '2', '3', '4']);
});

test('more than five sidebar sessions collapse to first five', () => {
  const sessions = Array.from({ length: 7 }, (_, i) => ({ id: String(i) }));
  const result = sidebarSessionProjection(sessions, false);
  assert.equal(result.collapsible, true);
  assert.equal(result.action, 'expand');
  assert.equal(result.hiddenCount, 2);
  assert.deepEqual(result.visibleSessions.map((s) => s.id), ['0', '1', '2', '3', '4']);
});

test('expanded sidebar sessions show all rows and collapse action', () => {
  const sessions = Array.from({ length: 7 }, (_, i) => ({ id: String(i) }));
  const result = sidebarSessionProjection(sessions, true);
  assert.equal(result.collapsible, true);
  assert.equal(result.action, 'collapse');
  assert.equal(result.hiddenCount, 0);
  assert.deepEqual(result.visibleSessions.map((s) => s.id), ['0', '1', '2', '3', '4', '5', '6']);
});

test('sortSidebarSessionsNewestFirst orders by updated then created time', () => {
  const result = sortSidebarSessionsNewestFirst([
    { id: 'old', updated_at: '2026-05-17T01:00:00Z' },
    { id: 'new', updated_at: '2026-05-17T03:00:00Z' },
    { id: 'middle', created_at: '2026-05-17T02:00:00Z' },
  ]);
  assert.deepEqual(result.map((s) => s.id), ['new', 'middle', 'old']);
});

test('upsertSidebarSession inserts new session newest-first', () => {
  const result = upsertSidebarSession([
    { id: 'old', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z' },
  ], {
    id: 'fork',
    workspace_hash: 'w1',
    updated_at: '2026-05-17T04:00:00Z',
  });
  assert.deepEqual(result.map((s) => s.id), ['fork', 'old']);
});

test('upsertSidebarSession replaces existing session without duplicates', () => {
  const result = upsertSidebarSession([
    { id: 'same', title: 'old', updated_at: '2026-05-17T01:00:00Z' },
    { id: 'other', updated_at: '2026-05-17T02:00:00Z' },
  ], {
    id: 'same',
    title: 'new',
    updated_at: '2026-05-17T03:00:00Z',
  });
  assert.deepEqual(result.map((s) => s.id), ['same', 'other']);
  assert.equal(result[0].title, 'new');
});
