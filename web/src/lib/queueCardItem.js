// 把 queue item 翻译成 QueueCardList 卡片要展示的结构。
// 抽出来是为了让 Node 单测能不依赖 DOM 验证状态/标签的映射。
//
// 输入是 chatInputQueue.js::buildQueuedMessageItems 返回的 item:
//   { kind:'msg', id, content, ts, queued: { id, sessionId, state, error, ... } }
// 输出是给 QueueCardList.jsx 一个稳定的 props 形状。

import { QUEUED_INPUT_STATE } from './chatInputQueue.js';

export function buildQueueCardItem(item) {
  const queued = item?.queued || {};
  const state = queued.state || QUEUED_INPUT_STATE.QUEUED;
  let statusLabel = '排队中';
  let statusKind = 'queued';
  let dimmed = false;
  let showRetry = false;
  if (state === QUEUED_INPUT_STATE.SENDING) {
    statusLabel = '发送中…';
    statusKind = 'sending';
    dimmed = true;
  } else if (state === QUEUED_INPUT_STATE.FAILED) {
    statusLabel = String(queued.error || '发送失败');
    statusKind = 'failed';
    showRetry = true;
  }
  return {
    queuedId: queued.id || '',
    content: String(item?.content || ''),
    state,
    statusLabel,
    statusKind,
    dimmed,
    showRetry,
  };
}

export function buildQueueCardItems(items) {
  return (Array.isArray(items) ? items : []).map(buildQueueCardItem);
}
