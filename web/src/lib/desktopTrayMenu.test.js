// desktopTrayMenu.js 单测。
//
// 验收点:
//   - buildTrayMenuPayload 不再预截断 pinned / recent
//   - pinned 按全局视觉顺序输出并去重
//   - recent 排除 pinned,按最近活动时间倒序输出
//   - 每项携带 workspace_hash 和 workspace 显示名副标题
//   - pushTrayMenu:无 bridge 时静默 no-op、debounce 100ms 合并多次调用
//
// 运行方式:`node web/src/lib/runTests.js`(同 desktopNotify.test.js 风格)。
// 设计:openspec/changes/enhance-desktop-tray-menu/design.md 决策 3。

import assert from 'node:assert/strict';
import {
  buildTrayMenuPayload,
  pushTrayMenu,
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
  const p = buildTrayMenuPayload({ sessions: [], pinnedByWorkspace: new Map(), workspaceName: 'ws' });
  assert.equal(p.pinned.length, 0);
  assert.equal(p.recent.length, 0);
  assert.equal(p.workspace_name, 'ws');
});

run('buildTrayMenuPayload pinned 按全局视觉顺序输出并带 workspace 副标题', () => {
  const sessions = [
    { id: 'a', title: 'A', workspace_hash: 'w1', updated_at: '2026-06-01T00:00:00Z' },
    { id: 'x', title: 'X', workspace_hash: 'w2', updated_at: '2026-06-03T00:00:00Z' },
    { id: 'y', title: 'Y', workspace_hash: 'w2', updated_at: '2026-06-02T00:00:00Z' },
  ];
  const p = buildTrayMenuPayload({
    sessions,
    pinnedByWorkspace: new Map([
      ['w1', ['a']],
      ['w2', ['x', 'y', 'x']],
    ]),
    pinnedOrderItems: [
      { workspace_hash: 'w2', session_id: 'y' },
      { workspace_hash: 'w1', session_id: 'a' },
      { workspace_hash: 'w2', session_id: 'x' },
    ],
    workspaces: [
      { hash: 'w1', name: 'acecode' },
      { hash: 'w2', name: 'shz_test' },
    ],
    workspaceName: 'active',
  });
  assert.deepEqual(p.pinned.map((item) => item.session_id), ['y', 'a', 'x']);
  assert.deepEqual(p.pinned.map((item) => item.subtitle), ['shz_test', 'acecode', 'shz_test']);
  assert.equal(p.pinned[0].workspace_hash, 'w2');
});

run('buildTrayMenuPayload recent 排除 pinned 并按 updated_at 倒序', () => {
  const sessions = [
    { id: 'p1', title: 'Pinned', workspace_hash: 'w1', updated_at: '2026-06-10T00:00:00Z' },
    { id: 'r-old', title: 'Old', workspace_hash: 'w1', updated_at: '2026-06-01T00:00:00Z' },
    { id: 'r-new', title: 'New', workspace_hash: 'w2', updated_at: '2026-06-12T00:00:00Z' },
    { id: 'r-mid', title: 'Mid', workspace_hash: 'w1', updated_at: '2026-06-05T00:00:00Z' },
  ];
  const p = buildTrayMenuPayload({
    sessions,
    pinnedByWorkspace: { w1: ['p1'] },
    pinnedOrderItems: [{ workspace_hash: 'w1', session_id: 'p1' }],
    workspaces: [
      { hash: 'w1', name: 'acecode' },
      { hash: 'w2', name: 'shz_test' },
    ],
    workspaceName: 'active',
  });
  assert.deepEqual(p.pinned.map((item) => item.session_id), ['p1']);
  assert.deepEqual(p.recent.map((item) => item.session_id), ['r-new', 'r-mid', 'r-old']);
  assert.deepEqual(p.recent.map((item) => item.subtitle), ['shz_test', 'acecode', 'acecode']);
});

run('buildTrayMenuPayload 不预截断 pinned / recent', () => {
  const sessions = [];
  const pinnedIds = [];
  for (let i = 0; i < 6; ++i) {
    sessions.push({
      id: 'p' + i,
      title: 'P' + i,
      workspace_hash: 'w',
      updated_at: `2026-06-${String(i + 1).padStart(2, '0')}T00:00:00Z`,
    });
    pinnedIds.push('p' + i);
  }
  for (let i = 0; i < 20; ++i) {
    sessions.push({
      id: 'r' + i,
      title: 'R' + i,
      workspace_hash: 'w',
      updated_at: `2026-07-${String(i + 1).padStart(2, '0')}T00:00:00Z`,
    });
  }
  const p = buildTrayMenuPayload({
    sessions,
    pinnedByWorkspace: new Map([['w', pinnedIds]]),
    pinnedOrderItems: pinnedIds.map((id) => ({ workspace_hash: 'w', session_id: id })),
    workspaces: [{ hash: 'w', name: 'acecode' }],
    workspaceName: 'acecode',
  });
  assert.equal(p.pinned.length, 6);
  assert.equal(p.recent.length, 20);
});

run('buildTrayMenuPayload legacy pinnedSessionIds fallback 仍可用', () => {
  const p = buildTrayMenuPayload({
    sessions: [
      { id: 's1', title: 'One', workspace_hash: 'w' },
      { id: 's2', title: 'Two', workspace_hash: 'w' },
    ],
    pinnedSessionIds: ['s2'],
    workspaceName: 'fallback-ws',
  });
  assert.deepEqual(p.pinned.map((item) => item.session_id), ['s2']);
  assert.deepEqual(p.recent.map((item) => item.session_id), ['s1']);
});

run('buildTrayMenuPayload 无 title 用新会话编号兜底', () => {
  const sessions = [
    { id: 's2', created_at: '2026-05-08T02:00:00Z' },
    { id: 's1', created_at: '2026-05-08T01:00:00Z' },
  ];
  const p = buildTrayMenuPayload({ sessions, pinnedByWorkspace: new Map(), workspaceName: '' });
  assert.equal(p.recent[0].title, '新会话2');
  assert.equal(p.recent[1].title, '新会话1');
});

run('buildTrayMenuPayload subtitle 超长截断到 SUBTITLE_LIMIT 内', () => {
  const longWorkspaceName = 'A'.repeat(80);
  const p = buildTrayMenuPayload({
    sessions: [{ id: 's1', title: 'T', workspace_hash: 'w' }],
    pinnedByWorkspace: new Map(),
    workspaces: [{ hash: 'w', name: longWorkspaceName }],
    workspaceName: '',
  });
  assert.ok(Array.from(p.recent[0].subtitle).length <= SUBTITLE_LIMIT);
  assert.ok(p.recent[0].subtitle.endsWith('…'));
});

run('buildTrayMenuPayload 无 id 的 session 跳过', () => {
  const p = buildTrayMenuPayload({
    sessions: [{ title: 'no-id' }, { id: 'has', title: 'X' }],
    pinnedByWorkspace: new Map(),
    workspaceName: '',
  });
  assert.equal(p.recent.length, 1);
  assert.equal(p.recent[0].session_id, 'has');
});

run('pushTrayMenu 无 bridge 时静默 no-op,不抛错', () => {
  __resetForTest();
  const prev = global.window;
  global.window = {}; // 无 aceDesktop_setTrayMenu
  __setTimerFnsForTest({
    setTimeout: (fn) => { fn(); return 1; },
    clearTimeout: () => {},
  });
  pushTrayMenu({ sessions: [{ id: 's1', title: 'X' }], pinnedByWorkspace: new Map(), workspaceName: '' });
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
  const captured = [];
  const prev = global.window;
  global.window = {
    aceDesktop_setTrayMenu: (json) => { captured.push(JSON.parse(json)); },
  };
  pushTrayMenu({ sessions: [{ id: 'a', title: 'A' }], pinnedByWorkspace: new Map(), workspaceName: 'w' });
  pushTrayMenu({ sessions: [{ id: 'b', title: 'B' }], pinnedByWorkspace: new Map(), workspaceName: 'w' });
  pushTrayMenu({ sessions: [{ id: 'c', title: 'C' }], pinnedByWorkspace: new Map(), workspaceName: 'w' });

  const peek = __peekQueuedPayloadForTest();
  assert.equal(peek.recent.length, 1);
  assert.equal(peek.recent[0].session_id, 'c');
  if (pendingFn) pendingFn();
  assert.equal(captured.length, 1);
  assert.equal(captured[0].recent[0].session_id, 'c');

  global.window = prev;
  __resetTimerFnsForTest();
  __resetForTest();
});
