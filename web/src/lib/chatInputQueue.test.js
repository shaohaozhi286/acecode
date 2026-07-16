import assert from 'node:assert/strict';
import {
  QUEUED_INPUT_STATE,
  acceptedQueuedInputEvent,
  beginQueuedGuidance,
  buildQueuedMessageItems,
  cancelQueuedInput,
  completeQueuedInputForMessage,
  createChatInputQueueState,
  enqueueQueuedInput,
  finishQueuedGuidance,
  markQueuedInputFailed,
  markQueuedInputSending,
  nextQueuedInput,
  queuedInputRequestPayload,
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

run('引导中项目暂停 FIFO，成功取消，失败恢复原状态', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'side question', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'later question', now: 101 });
  const id = nextQueuedInput(state, 's1').queued.id;

  state = beginQueuedGuidance(state, id);
  assert.equal(queuedInputsForSession(state, 's1')[0].queued.state, QUEUED_INPUT_STATE.GUIDING);
  assert.equal(nextQueuedInput(state, 's1'), null);

  state = finishQueuedGuidance(state, id, { succeeded: false });
  assert.equal(queuedInputsForSession(state, 's1')[0].queued.state, QUEUED_INPUT_STATE.QUEUED);
  assert.equal(nextQueuedInput(state, 's1').queued.id, id);

  state = beginQueuedGuidance(state, id);
  state = finishQueuedGuidance(state, id, { succeeded: true });
  assert.equal(queuedInputsForSession(state, 's1').length, 1);
  assert.equal(nextQueuedInput(state, 's1').content, 'later question');
});

run('附件 payload 可以在空文本时排队并保留发送体', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, {
    sessionId: 's1',
    payload: { text: '', attachments: [{ id: 'att_1' }], contexts: [] },
    now: 100,
  });
  const first = nextQueuedInput(state, 's1');
  assert.equal(first.content, '');
  assert.deepEqual(first.queued.payload.attachments, [{ id: 'att_1' }]);
});

run('排队提交复用稳定 id 作为请求关联键和接受后投影', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, {
    sessionId: 's1',
    payload: { text: '', attachments: [{ id: 'att_1' }], contexts: [] },
    now: 100,
  });
  const first = nextQueuedInput(state, 's1');

  assert.deepEqual(queuedInputRequestPayload(first), {
    text: '',
    attachments: [{ id: 'att_1' }],
    contexts: [],
    client_message_id: first.queued.id,
  });
  assert.deepEqual(acceptedQueuedInputEvent(first, { now: 250 }), {
    type: 'queued_input_accepted',
    payload: {
      client_message_id: first.queued.id,
      content: '附件消息',
    },
    timestamp_ms: 250,
  });
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

run('backend user message 优先按 client id 完成而不是误配相同文本', () => {
  let state = createChatInputQueueState();
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'same text', now: 100 });
  state = enqueueQueuedInput(state, { sessionId: 's1', text: 'same text', now: 101 });
  const [first, second] = queuedInputsForSession(state, 's1', { includeDone: true });
  state = markQueuedInputSending(state, first.queued.id, { now: 200 });

  const unchanged = completeQueuedInputForMessage(state, {
    sessionId: 's1',
    content: 'same text',
    ts: 250,
    clientMessageId: second.queued.id,
  });
  assert.equal(
    queuedInputsForSession(unchanged, 's1', { includeDone: true })[0].queued.state,
    QUEUED_INPUT_STATE.SENDING,
  );

  state = completeQueuedInputForMessage(state, {
    sessionId: 's1',
    content: 'expanded text can differ',
    ts: 250,
    clientMessageId: first.queued.id,
  });
  assert.equal(
    queuedInputsForSession(state, 's1', { includeDone: true })[0].queued.state,
    QUEUED_INPUT_STATE.COMPLETED,
  );
});
