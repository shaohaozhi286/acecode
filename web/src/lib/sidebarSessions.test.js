import assert from 'node:assert/strict';
import {
  SIDEBAR_SESSION_COLLAPSE_LIMIT,
  reconcileSidebarSessions,
  sessionListNeedsRevealExpansion,
  sessionMatchesRevealTarget,
  sidebarRevealTarget,
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

test('sidebarRevealTarget keeps workspace session identity', () => {
  assert.deepEqual(sidebarRevealTarget({
    sessionId: 's1',
    workspaceHash: 'w1',
  }), {
    sessionId: 's1',
    workspaceHash: 'w1',
    noWorkspace: false,
  });
});

test('sidebarRevealTarget marks no-workspace sessions without workspace hash', () => {
  assert.deepEqual(sidebarRevealTarget({
    session_id: 's1',
    workspace_hash: 'w1',
    no_workspace: true,
  }), {
    sessionId: 's1',
    workspaceHash: '',
    noWorkspace: true,
  });
});

test('sessionListNeedsRevealExpansion expands when target row is hidden', () => {
  const sessions = Array.from({ length: 7 }, (_, i) => ({
    id: String(i),
    workspace_hash: 'w1',
  }));
  assert.equal(sessionListNeedsRevealExpansion(sessions, {
    sessionId: '6',
    workspaceHash: 'w1',
  }, false), true);
  assert.equal(sessionListNeedsRevealExpansion(sessions, {
    sessionId: '3',
    workspaceHash: 'w1',
  }, false), false);
});

test('sessionMatchesRevealTarget separates workspace and no-workspace rows', () => {
  assert.equal(sessionMatchesRevealTarget({
    id: 's1',
    workspace_hash: 'w1',
  }, {
    sessionId: 's1',
    workspaceHash: 'w1',
  }), true);
  assert.equal(sessionMatchesRevealTarget({
    id: 's1',
    workspace_hash: 'w1',
  }, {
    sessionId: 's1',
    noWorkspace: true,
  }), false);
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
    { id: 'other', updated_at: '2026-05-17T02:00:00Z' },
    { id: 'same', title: 'old', updated_at: '2026-05-17T01:00:00Z' },
  ], {
    id: 'same',
    title: 'new',
    updated_at: '2026-05-17T03:00:00Z',
  });
  assert.deepEqual(result.map((s) => s.id), ['other', 'same']);
  assert.equal(result[1].title, 'new');
});

test('upsertSidebarSession promotes existing session only when content counters change', () => {
  const result = upsertSidebarSession([
    { id: 'other', updated_at: '2026-05-17T02:00:00Z' },
    { id: 'same', title: 'old', updated_at: '2026-05-17T01:00:00Z', message_count: 2 },
  ], {
    id: 'same',
    title: 'new',
    updated_at: '2026-05-17T03:00:00Z',
    message_count: 4,
  });
  assert.deepEqual(result.map((s) => s.id), ['same', 'other']);
  assert.equal(result[0].title, 'new');
});

test('reconcileSidebarSessions preserves row order when only updated_at changes', () => {
  const previous = [
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T02:00:00Z', message_count: 4, turn_count: 2 },
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T03:00:00Z', message_count: 6, turn_count: 3 },
  ];
  const incoming = [
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T09:00:00Z', message_count: 6, turn_count: 3 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T02:00:00Z', message_count: 4, turn_count: 2 },
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
  ];
  const result = reconcileSidebarSessions(previous, incoming);
  assert.deepEqual(result.map((s) => s.id), ['a', 'b', 'c']);
  assert.equal(result[2].updated_at, '2026-05-17T09:00:00Z');
});

test('reconcileSidebarSessions promotes content changes and new sessions', () => {
  const previous = [
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T02:00:00Z', message_count: 4, turn_count: 2 },
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T03:00:00Z', message_count: 6, turn_count: 3 },
  ];
  const incoming = [
    { id: 'a', workspace_hash: 'w1', updated_at: '2026-05-17T01:00:00Z', message_count: 2, turn_count: 1 },
    { id: 'b', workspace_hash: 'w1', updated_at: '2026-05-17T10:00:00Z', message_count: 8, turn_count: 3 },
    { id: 'c', workspace_hash: 'w1', updated_at: '2026-05-17T03:00:00Z', message_count: 6, turn_count: 3 },
    { id: 'new', workspace_hash: 'w1', updated_at: '2026-05-17T11:00:00Z', message_count: 0, turn_count: 0 },
  ];
  const result = reconcileSidebarSessions(previous, incoming);
  assert.deepEqual(result.map((s) => s.id), ['new', 'b', 'a', 'c']);
});
