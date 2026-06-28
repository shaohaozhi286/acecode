import assert from 'node:assert/strict';
import { createTranscriptState } from './sessionTranscript.js';
import {
  completedTurnSelfHealEnabled,
  createCompletedTurnSelfHealScheduler,
  findLatestUserTurnAnchor,
  reconcileLatestCompletedTurn,
} from './transcriptSelfHeal.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function msg(id, role, content, extra = {}) {
  return {
    kind: 'msg',
    id,
    messageId: extra.messageId || `${role}-${id}`,
    role,
    content,
    ts: id * 1000,
    ...extra,
  };
}

function user(id, content, messageId = `u-${id}`) {
  return msg(id, 'user', content, { messageId });
}

function assistant(id, content, messageId = `a-${id}`) {
  return msg(id, 'assistant', content, { messageId });
}

function tool(id, { object = `file-${id}.txt`, output = '', toolCallId = `call-${id}` } = {}) {
  return {
    kind: 'tool',
    id,
    ts: id * 1000,
    tool: {
      isDone: true,
      success: true,
      tool: 'file_read',
      toolCallId,
      summary: { verb: 'Read', object, metrics: [] },
      output,
      hunks: [],
    },
  };
}

function stateWith(items, extra = {}) {
  return createTranscriptState({
    items,
    nextItemId: 100,
    isLive: true,
    loadState: 'loaded',
    ...extra,
  });
}

function makeScheduler({
  enabled = true,
  live = true,
  sessionId = 's1',
  stateRef,
  fetchCanonicalHistory = async () => ({ messages: [], events: [] }),
  applyCanonicalHistory = () => ({ replaced: false, reason: 'signature_match' }),
} = {}) {
  const timers = [];
  let cleared = 0;
  const scheduler = createCompletedTurnSelfHealScheduler({
    delayMs: 1,
    retryDelayMs: 1,
    getEnabled: () => enabled,
    getSessionId: () => sessionId,
    getIsLive: () => live,
    getState: () => stateRef.current,
    isVisible: () => true,
    fetchCanonicalHistory,
    applyCanonicalHistory,
    setTimer: (fn) => {
      timers.push(fn);
      return timers.length;
    },
    clearTimer: () => {
      cleared += 1;
    },
  });
  return { scheduler, timers, cleared: () => cleared };
}

await run('self-heal health config defaults enabled and explicit health disables it', () => {
  assert.equal(completedTurnSelfHealEnabled(null), false);
  assert.equal(completedTurnSelfHealEnabled({}), true);
  assert.equal(completedTurnSelfHealEnabled({ features: {} }), true);
  assert.equal(completedTurnSelfHealEnabled({
    features: { completed_turn_self_heal: { enabled: true } },
  }), true);
  assert.equal(completedTurnSelfHealEnabled({
    features: { completed_turn_self_heal: { enabled: false } },
  }), false);
});

await run('latest user turn anchor prefers stable message id and falls back to ordinal content', () => {
  const byId = findLatestUserTurnAnchor([
    user(1, 'old', 'u-old'),
    assistant(2, 'done'),
    user(3, 'latest', 'u-latest'),
  ]);
  assert.equal(byId.key, 'user-id:u-latest');

  const fallback = findLatestUserTurnAnchor([
    { kind: 'msg', id: 1, role: 'user', content: 'same' },
    assistant(2, 'done'),
    { kind: 'msg', id: 3, role: 'user', content: 'same' },
  ]);
  assert.equal(fallback.key, 'user-ordinal:2:content:same');
});

await run('disabled config does not schedule or request canonical history', () => {
  let fetchCount = 0;
  const stateRef = {
    current: stateWith([user(1, 'work'), assistant(2, 'done')]),
  };
  const { scheduler, timers } = makeScheduler({
    enabled: false,
    stateRef,
    fetchCanonicalHistory: async () => {
      fetchCount += 1;
      return {};
    },
  });

  const result = scheduler.schedule();
  assert.equal(result.scheduled, false);
  assert.equal(result.reason, 'disabled');
  assert.equal(timers.length, 0);
  assert.equal(fetchCount, 0);
});

await run('matching latest turn signature is a no-op', () => {
  const current = stateWith([
    user(1, 'old'),
    assistant(2, 'old answer'),
    user(3, 'latest', 'u-latest'),
    assistant(4, 'canonical answer', 'a-live'),
  ]);
  const canonical = stateWith([
    user(10, 'old'),
    assistant(11, 'old answer'),
    user(12, 'latest', 'u-latest'),
    assistant(13, 'canonical answer', 'a-canonical'),
  ]);

  const result = reconcileLatestCompletedTurn(current, canonical);
  assert.equal(result.replaced, false);
  assert.equal(result.reason, 'signature_match');
  assert.equal(result.state, current);
});

await run('text mismatch replaces only the fragment after the latest user anchor', () => {
  const current = stateWith([
    user(1, 'old', 'u-old'),
    assistant(2, 'old answer', 'a-old'),
    user(3, 'latest', 'u-latest'),
    assistant(4, 'wrong live answer', 'a-live'),
  ]);
  const canonical = stateWith([
    user(10, 'old', 'u-old'),
    assistant(11, 'canonical old answer differs but is before latest user', 'a-old-canon'),
    user(12, 'latest', 'u-latest'),
    assistant(13, 'correct canonical answer', 'a-canonical'),
  ]);

  const result = reconcileLatestCompletedTurn(current, canonical);
  assert.equal(result.replaced, true);
  assert.deepEqual(result.state.items.map((item) => item.content), [
    'old',
    'old answer',
    'latest',
    'correct canonical answer',
  ]);
  assert.equal(result.state.items[2], current.items[2]);
  assert.equal(result.state.items[3].id, 100);
});

await run('tool activity mismatch replaces the whole latest assistant fragment', () => {
  const current = stateWith([
    user(1, 'inspect', 'u-latest'),
    tool(2, { object: 'wrong.txt', output: 'wrong output', toolCallId: 'call-read' }),
    assistant(3, 'done', 'a-live'),
  ]);
  const canonical = stateWith([
    user(10, 'inspect', 'u-latest'),
    tool(11, { object: 'right.txt', output: 'right output', toolCallId: 'call-read' }),
    assistant(12, 'done', 'a-canonical'),
  ]);

  const result = reconcileLatestCompletedTurn(current, canonical);
  assert.equal(result.replaced, true);
  assert.equal(result.state.items.length, 3);
  assert.equal(result.state.items[1].kind, 'tool');
  assert.equal(result.state.items[1].tool.summary.object, 'right.txt');
  assert.equal(result.state.items[1].tool.output, 'right output');
  assert.equal(result.state.items[2].content, 'done');
});

await run('stale self-heal result is discarded when latest user anchor changes before request', async () => {
  let fetchCount = 0;
  const stateRef = {
    current: stateWith([user(1, 'first', 'u-first'), assistant(2, 'done')]),
  };
  const { scheduler, timers } = makeScheduler({
    stateRef,
    fetchCanonicalHistory: async () => {
      fetchCount += 1;
      return {};
    },
  });

  assert.equal(scheduler.schedule().scheduled, true);
  stateRef.current = stateWith([user(3, 'second', 'u-second'), assistant(4, 'new done')]);
  timers[0]();
  await Promise.resolve();
  assert.equal(fetchCount, 0);
});

await run('duplicate completion signals for the same latest user turn coalesce', async () => {
  let fetchCount = 0;
  let applyCount = 0;
  const stateRef = {
    current: stateWith([user(1, 'work', 'u-work'), assistant(2, 'done')]),
  };
  const { scheduler, timers } = makeScheduler({
    stateRef,
    fetchCanonicalHistory: async () => {
      fetchCount += 1;
      return {};
    },
    applyCanonicalHistory: () => {
      applyCount += 1;
      return { replaced: false, reason: 'signature_match' };
    },
  });

  assert.equal(scheduler.schedule().scheduled, true);
  const duplicate = scheduler.schedule();
  assert.equal(duplicate.scheduled, false);
  assert.equal(duplicate.reason, 'duplicate_turn');
  assert.equal(timers.length, 1);

  timers[0]();
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(fetchCount, 1);
  assert.equal(applyCount, 1);
});
