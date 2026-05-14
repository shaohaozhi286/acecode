import assert from 'node:assert/strict';
import {
  SIDEBAR_SESSION_COLLAPSE_LIMIT,
  sidebarSessionProjection,
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
