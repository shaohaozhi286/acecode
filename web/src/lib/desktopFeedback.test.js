import assert from 'node:assert/strict';
import {
  NO_FEEDBACK_SESSION_KEY,
  buildDesktopFeedbackPayload,
  feedbackSessionKey,
  normalizeDesktopFeedbackSessions,
  selectedFeedbackSessionFromKey,
} from './desktopFeedback.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('desktop feedback normalizes sessions and removes duplicate options', () => {
  const sessions = normalizeDesktopFeedbackSessions({
    sessions: [
      { id: 's1', workspace_hash: 'w1', title: 'A' },
      { session_id: 's1', workspace_hash: 'w1', title: 'A duplicate' },
      { sessionId: 's2', workspaceHash: 'w2', title: 'B' },
      { title: 'missing id' },
    ],
  });

  assert.equal(sessions.length, 2);
  assert.equal(sessions[0].id, 's1');
  assert.equal(sessions[0].workspace_hash, 'w1');
  assert.equal(sessions[1].id, 's2');
  assert.equal(sessions[1].workspace_hash, 'w2');
});

run('desktop feedback session key preserves no-session default', () => {
  assert.equal(feedbackSessionKey(null), NO_FEEDBACK_SESSION_KEY);
  const session = { id: 'sid/a', workspace_hash: 'ws/b' };
  const key = feedbackSessionKey(session);
  assert.notEqual(key, NO_FEEDBACK_SESSION_KEY);
  assert.deepEqual(selectedFeedbackSessionFromKey([session], key), {
    id: 'sid/a',
    session_id: 'sid/a',
    workspace_hash: 'ws/b',
  });
  assert.equal(selectedFeedbackSessionFromKey([session], ''), null);
});

run('desktop feedback payload omits session fields by default', () => {
  assert.deepEqual(buildDesktopFeedbackPayload({ feedbackText: 'log only' }), {
    feedback_text: 'log only',
  });
});

run('desktop feedback payload includes exactly one selected session', () => {
  assert.deepEqual(
    buildDesktopFeedbackPayload({
      feedbackText: 'attach this',
      selectedSession: { id: 's1', workspace_hash: 'w1' },
    }),
    {
      feedback_text: 'attach this',
      session_id: 's1',
      workspace_hash: 'w1',
    },
  );
});
