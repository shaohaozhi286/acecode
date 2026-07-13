import assert from 'node:assert/strict';
import {
  goBack,
  goForward,
  navigationKey,
  pushNavigation,
  sameNavigationRef,
} from './navigationHistory.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('navigationKey distinguishes home and sessions', () => {
  assert.notEqual(
    navigationKey({ home: true, workspaceHash: 'w1', cwd: '/repo' }),
    navigationKey({ workspaceHash: 'w1', sessionId: 's1', cwd: '/repo' }),
  );
  assert.equal(
    navigationKey({ workspace_hash: 'w1', session_id: 's1', cwd: '/repo' }),
    navigationKey({ workspaceHash: 'w1', sessionId: 's1', cwd: '/repo' }),
  );
});

run('LOOP is a first-class navigation destination', () => {
  const session = { workspaceHash: 'w1', sessionId: 's1' };
  const loop = { loop: true };
  assert.notEqual(navigationKey(loop), navigationKey(session));
  const opened = pushNavigation({ back: [], forward: [] }, session, loop);
  const back = goBack(opened, loop);
  assert.deepEqual(back.activeRef, session);
  assert.deepEqual(goForward(back.history, session).activeRef, loop);
});

run('sameNavigationRef ignores display-only fields', () => {
  assert.equal(
    sameNavigationRef(
      { workspaceHash: 'w1', sessionId: 's1', displayTitle: 'Old' },
      { workspaceHash: 'w1', sessionId: 's1', displayTitle: 'New' },
    ),
    true,
  );
});

run('pushNavigation records current ref and clears forward stack', () => {
  const current = { workspaceHash: 'w1', sessionId: 'a' };
  const next = { workspaceHash: 'w1', sessionId: 'b' };
  const history = pushNavigation({ back: [], forward: [{ sessionId: 'future' }] }, current, next);
  assert.deepEqual(history.back, [current]);
  assert.deepEqual(history.forward, []);
});

run('goBack and goForward traverse active refs', () => {
  const a = { workspaceHash: 'w1', sessionId: 'a' };
  const b = { workspaceHash: 'w1', sessionId: 'b' };
  const c = { workspaceHash: 'w1', sessionId: 'c' };
  const history = { back: [a, b], forward: [] };

  const backResult = goBack(history, c);
  assert.deepEqual(backResult.activeRef, b);
  assert.deepEqual(backResult.history.back, [a]);
  assert.deepEqual(backResult.history.forward, [c]);

  const forwardResult = goForward(backResult.history, b);
  assert.deepEqual(forwardResult.activeRef, c);
  assert.deepEqual(forwardResult.history.back, [a, b]);
  assert.deepEqual(forwardResult.history.forward, []);
});
