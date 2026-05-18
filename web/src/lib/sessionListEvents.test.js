import assert from 'node:assert/strict';
import {
  SESSION_LIST_CHANGED_EVENT,
  normalizeSessionListChangedDetail,
  notifySessionListChanged,
} from './sessionListEvents.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('normalizeSessionListChangedDetail reads session and workspace aliases', () => {
  const detail = normalizeSessionListChangedDetail({
    reason: 'fork',
    session: { id: 's1', workspace_hash: 'w1' },
  });
  assert.equal(detail.sessionId, 's1');
  assert.equal(detail.workspaceHash, 'w1');
  assert.equal(detail.reason, 'fork');
});

test('notifySessionListChanged dispatches normalized detail', () => {
  const seen = [];
  const target = {
    dispatchEvent(event) {
      seen.push(event);
      return true;
    },
  };
  const returned = notifySessionListChanged({
    session_id: 's2',
    workspace_hash: 'w2',
  }, target);
  assert.equal(returned.sessionId, 's2');
  assert.equal(returned.workspaceHash, 'w2');
  assert.equal(seen.length, 1);
  assert.equal(seen[0].type, SESSION_LIST_CHANGED_EVENT);
  assert.equal(seen[0].detail.sessionId, 's2');
});
