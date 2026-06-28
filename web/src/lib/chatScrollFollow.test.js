import assert from 'node:assert/strict';
import {
  CHAT_TAIL_FOLLOW_STATE,
  chatScrollMetrics,
  chatTailDistance,
  isChatNearTail,
  nextChatTailFollowState,
  shouldAutoFollowChatTail,
} from './chatScrollFollow.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('at-tail metrics are near the chat tail', () => {
  assert.equal(chatTailDistance({ scrollTop: 520, clientHeight: 400, scrollHeight: 1000 }), 80);
  assert.equal(isChatNearTail({ scrollTop: 520, clientHeight: 400, scrollHeight: 1000 }, 80), true);
});

run('away-from-tail metrics are not near the chat tail', () => {
  assert.equal(chatTailDistance({ scrollTop: 360, clientHeight: 400, scrollHeight: 1000 }), 240);
  assert.equal(isChatNearTail({ scrollTop: 360, clientHeight: 400, scrollHeight: 1000 }, 80), false);
});

run('scrolling away pauses tail follow', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, {
    type: 'scroll',
    metrics: { scrollTop: 100, clientHeight: 400, scrollHeight: 1000 },
    thresholdPx: 80,
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.REVIEWING);
  assert.equal(shouldAutoFollowChatTail(state), false);
});

run('returning near the tail resumes tail follow', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, {
    type: 'scroll',
    metrics: { scrollTop: 525, clientHeight: 400, scrollHeight: 1000 },
    thresholdPx: 80,
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
  assert.equal(shouldAutoFollowChatTail(state), true);
});

run('explicit review pause wins even if the viewport is near the tail', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, {
    type: 'review_pause',
    metrics: { scrollTop: 530, clientHeight: 400, scrollHeight: 1000 },
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.REVIEWING);
});

run('session reset and new turn initialize tail follow', () => {
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, { type: 'session_reset' }),
    CHAT_TAIL_FOLLOW_STATE.FOLLOWING,
  );
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, { type: 'new_turn' }),
    CHAT_TAIL_FOLLOW_STATE.FOLLOWING,
  );
});

run('self-heal replacement preserves the current tail-follow mode', () => {
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, { type: 'self_heal_replace' }),
    CHAT_TAIL_FOLLOW_STATE.REVIEWING,
  );
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, { type: 'self_heal_replace' }),
    CHAT_TAIL_FOLLOW_STATE.FOLLOWING,
  );
});

run('scroll metrics normalize invalid values', () => {
  assert.deepEqual(chatScrollMetrics({ scrollTop: -1, clientHeight: 'abc', scrollHeight: 120 }), {
    scrollTop: 0,
    clientHeight: 0,
    scrollHeight: 120,
  });
});
