// Sticky user context helper 单元测试:覆盖滚动位置到当前用户问题的纯逻辑映射。

import assert from 'node:assert/strict';
import { findStickyUserContext, sameStickyUserContext } from './stickyUserContext.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const items = [
  { id: 1, kind: 'msg', role: 'user', messageId: 'u1', content: '第一个问题' },
  { id: 2, kind: 'msg', role: 'assistant', content: '回答一' },
  { id: 3, kind: 'tool', tool: { title: '工具输出' } },
  { id: 4, kind: 'msg', role: 'user', messageId: 'u2', content: '第二个问题' },
  { id: 5, kind: 'msg', role: 'assistant', content: '回答二' },
];

const rowMetrics = [
  { id: 1, top: 0, bottom: 80 },
  { id: 2, top: 92, bottom: 260 },
  { id: 3, top: 272, bottom: 390 },
  { id: 4, top: 420, bottom: 490 },
  { id: 5, top: 502, bottom: 760 },
];

run('工具和回答区域使用最近的前置用户问题', () => {
  const context = findStickyUserContext({
    items,
    rowMetrics,
    scrollTop: 180,
    clientHeight: 260,
    scrollHeight: 900,
  });
  assert.deepEqual(context, {
    itemId: 1,
    messageId: 'u1',
    content: '第一个问题',
  });
});

run('滚动到下一轮后切换到新的用户问题', () => {
  const context = findStickyUserContext({
    items,
    rowMetrics,
    scrollTop: 500,
    clientHeight: 260,
    scrollHeight: 1100,
  });
  assert.deepEqual(context, {
    itemId: 4,
    messageId: 'u2',
    content: '第二个问题',
  });
});

run('源用户问题仍可见时隐藏 sticky 上下文', () => {
  const context = findStickyUserContext({
    items,
    rowMetrics,
    scrollTop: 20,
    clientHeight: 260,
    scrollHeight: 900,
  });
  assert.equal(context, null);
});

run('接近底部时隐藏 sticky 上下文', () => {
  const context = findStickyUserContext({
    items,
    rowMetrics,
    scrollTop: 680,
    clientHeight: 260,
    scrollHeight: 920,
  });
  assert.equal(context, null);
});

run('没有前置用户问题时不显示 sticky 上下文', () => {
  const context = findStickyUserContext({
    items: items.slice(1),
    rowMetrics: rowMetrics.slice(1),
    scrollTop: 140,
    clientHeight: 260,
    scrollHeight: 900,
  });
  assert.equal(context, null);
});

run('sticky 上下文相等比较使用来源和内容', () => {
  assert.equal(sameStickyUserContext(null, null), true);
  assert.equal(sameStickyUserContext({ itemId: 1, messageId: 'u1', content: 'a' }, { itemId: 1, messageId: 'u1', content: 'a' }), true);
  assert.equal(sameStickyUserContext({ itemId: 1, messageId: 'u1', content: 'a' }, { itemId: 1, messageId: 'u1', content: 'b' }), false);
});
