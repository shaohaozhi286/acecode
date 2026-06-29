import assert from 'node:assert/strict';
import {
  desktopOpenSessionUrl,
  openSessionTargetFromSearch,
  sessionRefFromJumpTarget,
  stripOpenSessionParams,
} from './sessionJump.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('open session search params preserve workspace identity', () => {
  const target = openSessionTargetFromSearch('?token=t1&open=s1&workspace=w1');
  assert.deepEqual(target, { sessionId: 's1', workspaceHash: 'w1', noWorkspace: false });
  assert.equal(stripOpenSessionParams('?token=t1&open=s1&workspace=w1'), 'token=t1');
});

test('desktop open session URL carries workspace hash to landing page', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok+/=',
    sessionId: 's1',
    workspaceHash: 'w hash',
    protocol: 'http:',
  });
  assert.equal(url, 'http://127.0.0.1:4567/?token=tok%2B%2F%3D&open=s1&workspace=w+hash');
});

test('desktop open session URL can target no-workspace sessions', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok',
    sessionId: 's1',
    noWorkspace: true,
  });
  assert.equal(url, 'http://127.0.0.1:4567/?token=tok&open=s1&no_workspace=1');
});

test('session ref from jump target merges resume result and search metadata', () => {
  const ref = sessionRefFromJumpTarget(
    {
      id: 's1',
      workspace_hash: 'w-search',
      display_title: 'Search title',
      message_count: 3,
    },
    {
      session_id: 's1',
      workspace_hash: 'w-resumed',
      cwd: 'N:/repo',
      active: true,
    },
  );
  assert.equal(ref.sessionId, 's1');
  assert.equal(ref.workspaceHash, 'w-resumed');
  assert.equal(ref.cwd, 'N:/repo');
  assert.equal(ref.displayTitle, 'Search title');
  assert.equal(ref.message_count, 3);
  assert.equal(ref.contextId, 'default');
});
