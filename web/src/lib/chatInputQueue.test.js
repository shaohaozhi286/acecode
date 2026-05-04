import assert from 'node:assert/strict';
import {
  QUEUED_INPUT_STATE,
  buildQueuedMessageItems,
  cancelQueuedInput,
  completeQueuedInputForMessage,
  createChatInputQueueState,
  enqueueQueuedInput,
  markQueuedInputFailed,
  markQueuedInputSending,
  nextQueuedInput,
  queuedInputsForSession,
  retryQueuedInput,
} from './chatInputQueue.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('enqueue 创建稳定本地消息并按 session 隔离', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'first', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's2', text: 'second', now: 101 });

  const s1 = queuedInputsForSession(state, 's1');
  const s2 = queuedInputsForSession(state, 's2');
  assert.equal(s1.length, 1);
  assert.equal(s2.length, 1);
  assert.equal(s1[0].kind, 'msg');
  assert.equal(s1[0].role, 'user');
  assert.equal(s1[0].content, 'first');
  assert.equal(s1[0].queued.state, QUEUED_INPUT_STATE.QUEUED);
  assert.match(s1[0].queued.id, /^queued-s1-1$/);
  assert.match(s2[0].queued.id, /^queued-s2-2$/);
});

run('nextQueuedInput 保持 FIFO 且发送中时不取下一条', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'one', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'two', now: 101 });
  const first = nextQueuedInput(state, 's1');
  assert.equal(first.content, 'one');

  state = markQueuedInputSending(state, first.queued.id, { now: 200 });
  assert.equal(nextQueuedInput(state, 's1'), null);
});

run('cancelled 项不会出现在可见队列也不会被发送', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'one' });
  const first = nextQueuedInput(state, 's1');
  state = cancelQueuedInput(state, first.queued.id);

  assert.equal(queuedInputsForSession(state, 's1').length, 0);
  assert.equal(nextQueuedInput(state, 's1'), null);
});

run('failed 项保留可见状态并可重试', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'one' });
  const first = nextQueuedInput(state, 's1');
  state = markQueuedInputSending(state, first.queued.id, { now: 200 });
  state = markQueuedInputFailed(state, first.queued.id, 'network');

  let visible = buildQueuedMessageItems(state, 's1');
  assert.equal(visible.length, 1);
  assert.equal(visible[0].queued.state, QUEUED_INPUT_STATE.FAILED);
  assert.equal(visible[0].queued.error, 'network');
  assert.equal(nextQueuedInput(state, 's1'), null);

  state = retryQueuedInput(state, first.queued.id);
  assert.equal(nextQueuedInput(state, 's1').content, 'one');
});

run('backend user message 到达后完成对应 sending 占位', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'same text', now: 100 });
  const first = nextQueuedInput(state, 's1');
  state = markQueuedInputSending(state, first.queued.id, { now: 200 });
  state = completeQueuedInputForMessage(state, { sessionId: 's1', content: 'same text', ts: 250 });

  assert.equal(queuedInputsForSession(state, 's1').length, 0);
  assert.equal(queuedInputsForSession(state, 's1', { includeDone: true })[0].queued.state, QUEUED_INPUT_STATE.COMPLETED);
});
