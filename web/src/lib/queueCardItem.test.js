// queueCardItem.js 的单元测试。
//
// 项目内 JS 测试只跑 Node + node:assert,没有 JSDOM / RTL,所以把 QueueCardList 的
// 状态↔标签映射逻辑抽到纯函数 buildQueueCardItem 里测;DOM 端只是把这份结构
// 映射到 className,无独立行为可测。
//
// 覆盖:
//  - 空 items / 缺失 queued 的容错
//  - QUEUED → "排队中",非 dimmed,无 retry
//  - SENDING → "发送中…",dimmed,无 retry
//  - FAILED → 优先用 error 文案,有 retry
//  - buildQueueCardItems 保持 FIFO 顺序

import assert from 'node:assert/strict';
import { QUEUED_INPUT_STATE } from './chatInputQueue.js';
import { buildQueueCardItem, buildQueueCardItems } from './queueCardItem.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function makeItem(state, content = 'hi', error = '') {
  return {
    kind: 'msg',
    id: `queued-s-${state}`,
    role: 'user',
    content,
    queued: {
      id: `queued-s-${state}`,
      sessionId: 's',
      state,
      error,
      payload: { text: content, attachments: [], contexts: [] },
    },
  };
}

run('buildQueueCardItem QUEUED 默认排队中,无 retry', () => {
  const card = buildQueueCardItem(makeItem(QUEUED_INPUT_STATE.QUEUED, 'hello'));
  assert.equal(card.queuedId, 'queued-s-queued');
  assert.equal(card.content, 'hello');
  assert.equal(card.statusKind, 'queued');
  assert.equal(card.statusLabel, '排队中');
  assert.equal(card.dimmed, false);
  assert.equal(card.showRetry, false);
  assert.equal(card.canGuide, true);
});

run('buildQueueCardItem SENDING 是 dimmed 的发送中状态', () => {
  const card = buildQueueCardItem(makeItem(QUEUED_INPUT_STATE.SENDING));
  assert.equal(card.statusKind, 'sending');
  assert.equal(card.statusLabel, '发送中…');
  assert.equal(card.dimmed, true);
  assert.equal(card.showRetry, false);
  assert.equal(card.canGuide, false);
});

run('buildQueueCardItem FAILED 显示 error 文案并允许重试', () => {
  const card = buildQueueCardItem(
    makeItem(QUEUED_INPUT_STATE.FAILED, 'hi', 'network blew up'),
  );
  assert.equal(card.statusKind, 'failed');
  assert.equal(card.statusLabel, 'network blew up');
  assert.equal(card.dimmed, false);
  assert.equal(card.showRetry, true);
  assert.equal(card.canGuide, true);
});

run('buildQueueCardItem FAILED 缺 error 时回退默认文案', () => {
  const card = buildQueueCardItem(makeItem(QUEUED_INPUT_STATE.FAILED, 'hi', ''));
  assert.equal(card.statusLabel, '发送失败');
  assert.equal(card.showRetry, true);
});

run('buildQueueCardItem GUIDING 显示引导中且不重复显示按钮', () => {
  const card = buildQueueCardItem(makeItem(QUEUED_INPUT_STATE.GUIDING, 'hi'));
  assert.equal(card.statusKind, 'guiding');
  assert.equal(card.statusLabel, '引导中…');
  assert.equal(card.dimmed, true);
  assert.equal(card.canGuide, false);
});

run('附件或上下文排队项不显示引导按钮', () => {
  const attachmentItem = makeItem(QUEUED_INPUT_STATE.QUEUED, 'with file');
  attachmentItem.queued.payload.attachments = [{ id: 'att-1' }];
  assert.equal(buildQueueCardItem(attachmentItem).canGuide, false);

  const contextItem = makeItem(QUEUED_INPUT_STATE.FAILED, 'with context');
  contextItem.queued.payload.contexts = [{ type: 'selection' }];
  assert.equal(buildQueueCardItem(contextItem).canGuide, false);
});

run('buildQueueCardItem 缺 queued 字段时不崩溃,默认按 QUEUED 渲染', () => {
  const card = buildQueueCardItem({ kind: 'msg', id: '1', content: 'x' });
  assert.equal(card.queuedId, '');
  assert.equal(card.statusKind, 'queued');
  assert.equal(card.statusLabel, '排队中');
});

run('buildQueueCardItems 处理 null/非数组并保持 FIFO 顺序', () => {
  assert.deepEqual(buildQueueCardItems(null), []);
  assert.deepEqual(buildQueueCardItems(undefined), []);
  assert.deepEqual(buildQueueCardItems('not array'), []);

  const cards = buildQueueCardItems([
    makeItem(QUEUED_INPUT_STATE.SENDING, 'first'),
    makeItem(QUEUED_INPUT_STATE.QUEUED, 'second'),
    makeItem(QUEUED_INPUT_STATE.FAILED, 'third', 'boom'),
  ]);
  assert.equal(cards.length, 3);
  assert.equal(cards[0].content, 'first');
  assert.equal(cards[1].content, 'second');
  assert.equal(cards[2].content, 'third');
  assert.equal(cards[0].statusKind, 'sending');
  assert.equal(cards[1].statusKind, 'queued');
  assert.equal(cards[2].statusKind, 'failed');
});

run('buildQueueCardItem 长文本完整保留,UI 端用 CSS 截断 + title', () => {
  const longText = '一'.repeat(2000);
  const card = buildQueueCardItem(makeItem(QUEUED_INPUT_STATE.QUEUED, longText));
  // 数据层不截断 — 完整文本传到 DOM,truncation 由 CSS 完成,title 保留全文
  assert.equal(card.content, longText);
  assert.equal(card.content.length, 2000);
});
