// desktopTrayMenu.js 单测。
//
// 验收点:
//   - buildTrayMenuPayload:pinned 上限 5 / recent 上限 14、subtitle 截断 40
//   - 空标题会话展示为 "新会话N",不透出 GUID
//   - sessions 中混合 pinned + recent 时,pinned 取 pinnedSessionIds 内项,
//     recent 取剩下的前 14 条
//   - workspaceName 作为 fallback subtitle 注入
//   - pushTrayMenu:无 bridge 时静默 no-op、debounce 100ms 合并多次调用
//
// 运行方式:`node web/src/lib/runTests.js`(同 desktopNotify.test.js 风格)。
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3。

import assert from 'node:assert/strict';
import {
  buildTrayMenuPayload,
  pushTrayMenu,
  PINNED_LIMIT,
  RECENT_LIMIT_INCLUDING_MORE,
  SUBTITLE_LIMIT,
  __setTimerFnsForTest,
  __resetTimerFnsForTest,
  __peekQueuedPayloadForTest,
  __resetForTest,
} from './desktopTrayMenu.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('buildTrayMenuPayload 空 sessions → 空 pinned / 空 recent', () => {
  const p = buildTrayMenuPayload({ sessions: [], pinnedSessionIds: [], workspaceName: 'ws' });
  assert.equal(p.pinned.length, 0);
  assert.equal(p.recent.length, 0);
  assert.equal(p.workspace_name, 'ws');
});

run('buildTrayMenuPayload pinned + recent 切片', () => {
  const sessions = [
    { id: 's1', title: 'Hello' },
    { id: 's2', title: 'World' },
    { id: 's3', title: 'Foo' },
  ];
  const p = buildTrayMenuPayload({
    sessions,
    pinnedSessionIds: ['s2'],
    workspaceName: 'acecode',
  });
  assert.equal(p.pinned.length, 1);
  assert.equal(p.pinned[0].session_id, 's2');
  assert.equal(p.recent.length, 2);
  assert.equal(p.recent[0].session_id, 's1');
  assert.equal(p.recent[1].session_id, 's3');
});

run('buildTrayMenuPayload pinned 超 5 → 截到 5', () => {
  const sessions = [];
  const pinIds = [];
  for (let i = 0; i < 12; ++i) {
    sessions.push({ id: 'p' + i, title: 'P' + i });
    pinIds.push('p' + i);
  }
  const p = buildTrayMenuPayload({ sessions, pinnedSessionIds: pinIds, workspaceName: '' });
  assert.equal(p.pinned.length, PINNED_LIMIT);
  assert.equal(p.recent.length, 0);
});

run('buildTrayMenuPayload recent 超 14 → 截到 14', () => {
  const sessions = [];
  for (let i = 0; i < 30; ++i) sessions.push({ id: 'r' + i, title: 'R' + i });
  const p = buildTrayMenuPayload({ sessions, pinnedSessionIds: [], workspaceName: '' });
  assert.equal(p.recent.length, RECENT_LIMIT_INCLUDING_MORE);
});

run('buildTrayMenuPayload 无 title 用新会话编号兜底', () => {
  const sessions = [
    { id: 's2', created_at: '2026-05-08T02:00:00Z' },
    { id: 's1', created_at: '2026-05-08T01:00:00Z' },
  ];
  const p = buildTrayMenuPayload({ sessions, pinnedSessionIds: [], workspaceName: '' });
  assert.equal(p.recent[0].title, '新会话2');
  assert.equal(p.recent[1].title, '新会话1');
});

run('buildTrayMenuPayload subtitle 优先 summary,fallback workspaceName', () => {
  const p = buildTrayMenuPayload({
    sessions: [
      { id: 's1', title: 'T1', summary: 'sum1' },
      { id: 's2', title: 'T2' }, // 无 summary
    ],
    pinnedSessionIds: [],
    workspaceName: 'fallback-ws',
  });
  assert.equal(p.recent[0].subtitle, 'sum1');
  assert.equal(p.recent[1].subtitle, 'fallback-ws');
});

run('buildTrayMenuPayload subtitle 超长截断到 SUBTITLE_LIMIT 内', () => {
  const longSummary = 'A'.repeat(80);
  const p = buildTrayMenuPayload({
    sessions: [{ id: 's1', title: 'T', summary: longSummary }],
    pinnedSessionIds: [],
    workspaceName: '',
  });
  // codepoint 长度 ≤ SUBTITLE_LIMIT
  assert.ok(Array.from(p.recent[0].subtitle).length <= SUBTITLE_LIMIT);
  assert.ok(p.recent[0].subtitle.endsWith('…'));
});

run('buildTrayMenuPayload 无 id 的 session 跳过', () => {
  const p = buildTrayMenuPayload({
    sessions: [{ title: 'no-id' }, { id: 'has', title: 'X' }],
    pinnedSessionIds: [],
    workspaceName: '',
  });
  assert.equal(p.recent.length, 1);
  assert.equal(p.recent[0].session_id, 'has');
});

run('pushTrayMenu 无 bridge 时静默 no-op,不抛错', () => {
  __resetForTest();
  const prev = global.window;
  global.window = {}; // 无 aceDesktop_setTrayMenu
  // 用同步定时器:立即 fire
  __setTimerFnsForTest({
    setTimeout: (fn) => { fn(); return 1; },
    clearTimeout: () => {},
  });
  pushTrayMenu({ sessions: [{ id: 's1', title: 'X' }], pinnedSessionIds: [], workspaceName: '' });
  global.window = prev;
  __resetTimerFnsForTest();
  __resetForTest();
});

run('pushTrayMenu debounce:多次连续调用合并成最后一次 payload', () => {
  __resetForTest();
  let pendingFn = null;
  let pendingId = 0;
  __setTimerFnsForTest({
    setTimeout: (fn) => { pendingFn = fn; pendingId += 1; return pendingId; },
    clearTimeout: () => { pendingFn = null; },
  });
  let captured = [];
  const prev = global.window;
  global.window = {
    aceDesktop_setTrayMenu: (json) => { captured.push(JSON.parse(json)); },
  };
  pushTrayMenu({ sessions: [{ id: 'a', title: 'A' }], pinnedSessionIds: [], workspaceName: 'w' });
  pushTrayMenu({ sessions: [{ id: 'b', title: 'B' }], pinnedSessionIds: [], workspaceName: 'w' });
  pushTrayMenu({ sessions: [{ id: 'c', title: 'C' }], pinnedSessionIds: [], workspaceName: 'w' });
  // pending 之前未 flush — 期望 _lastQueuedPayload 是最新的 c
  const peek = __peekQueuedPayloadForTest();
  assert.equal(peek.recent.length, 1);
  assert.equal(peek.recent[0].session_id, 'c');
  // 真正触发一次 flush
  if (pendingFn) pendingFn();
  // 期望 native 收到一次,且是 c
  assert.equal(captured.length, 1);
  assert.equal(captured[0].recent[0].session_id, 'c');
  global.window = prev;
  __resetTimerFnsForTest();
  __resetForTest();
});
