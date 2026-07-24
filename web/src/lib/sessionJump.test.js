import assert from 'node:assert/strict';
import {
  desktopOpenSessionUrl,
  openSessionTargetFromSearch,
  sessionJumpMessageOrdinal,
  sessionJumpReadOnly,
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
  assert.deepEqual(target, {
    sessionId: 's1',
    workspaceHash: 'w1',
    noWorkspace: false,
    readOnly: false,
  });
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

test('desktop open session URL preserves explicit TUI-owned read-only mode', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok',
    sessionId: 's1',
    workspaceHash: 'w1',
    readOnly: true,
  });
  assert.equal(
    url,
    'http://127.0.0.1:4567/?token=tok&open=s1&workspace=w1&read_only=1',
  );

  const target = openSessionTargetFromSearch(
    '?token=tok&open=s1&workspace=w1&read_only=1',
  );
  assert.equal(sessionJumpReadOnly(target), true);
  assert.equal(stripOpenSessionParams(
    '?token=tok&open=s1&workspace=w1&read_only=1',
  ), 'token=tok');
  const ref = sessionRefFromJumpTarget(target);
  assert.equal(ref.readOnly, true);
  assert.equal(ref.externalSurface, 'tui');
});

test('session ref from jump target merges resume result and search metadata', () => {
  const ref = sessionRefFromJumpTarget(
    {
      id: 's1',
      workspace_hash: 'w-search',
      display_title: 'Search title',
      message_count: 3,
      search_match: { kind: 'user_message', message_ordinal: 7, snippet: 'needle' },
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
  assert.equal(ref.searchMatch.messageOrdinal, 7);
  assert.equal(ref.searchMatch.message_ordinal, 7);
  assert.equal(ref.searchMatch.snippet, 'needle');
});

test('desktop open session URL preserves matched message ordinal', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok',
    sessionId: 's1',
    workspaceHash: 'w1',
    messageOrdinal: 12,
  });
  assert.equal(url, 'http://127.0.0.1:4567/?token=tok&open=s1&workspace=w1&message_ordinal=12');

  const target = openSessionTargetFromSearch('?open=s1&workspace=w1&message_ordinal=12');
  assert.equal(sessionJumpMessageOrdinal(target), 12);
  assert.deepEqual(target, {
    sessionId: 's1',
    workspaceHash: 'w1',
    noWorkspace: false,
    readOnly: false,
    search_match: { kind: 'user_message', message_ordinal: 12, messageOrdinal: 12 },
  });
  assert.equal(stripOpenSessionParams('?token=t1&open=s1&workspace=w1&message_ordinal=12'), 'token=t1');
});
