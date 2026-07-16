// desktopNotify.js 单测。
//
// 项目无 JSDOM,只测纯函数(buildNotificationPayload / truncateForNotification /
// shouldSuppress)。notify / maybeNotify 依赖 window.aceDesktop_notify 桥,
// 通过最小 stub 注入测。
//
// 设计参见 openspec/changes/add-windows-wintoast-completion-notifications/design.md。

import assert from 'node:assert/strict';
import {
  buildNotificationPayload,
  shouldSuppress,
  truncateForNotification,
  maybeNotify,
} from './desktopNotify.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('truncateForNotification 短文本不变', () => {
  assert.equal(truncateForNotification('hello'), 'hello');
  assert.equal(truncateForNotification(''), '');
});

run('truncateForNotification 超长按 codepoint 截断 + 省略号', () => {
  const long = 'a'.repeat(120);
  const out = truncateForNotification(long);
  assert.equal(Array.from(out).length, 81); // 80 + …
  assert.ok(out.endsWith('…'));
});

run('truncateForNotification 中文 emoji 按 codepoint 不破坏字符', () => {
  // 100 个中文,limit 80
  const cn = '一'.repeat(100);
  const out = truncateForNotification(cn);
  assert.equal(Array.from(out).length, 81);
  assert.ok(out.endsWith('…'));
  // emoji surrogate 不被截到一半
  const emoji = '🚀'.repeat(50);
  const out2 = truncateForNotification(emoji, 5);
  assert.equal(Array.from(out2).length, 6); // 5 emoji + …
});

run('buildNotificationPayload question 类型基础形态', () => {
  const p = buildNotificationPayload({
    type: 'question',
    sessionId: 's1',
    workspaceHash: 'w1',
    sessionTitle: '我的会话',
    bodyText: '请确认是否继续',
  });
  assert.equal(p.title, '需要你回答 · 我的会话');
  assert.equal(p.body, '请确认是否继续');
  assert.equal(p.session_id, 's1');
  assert.equal(p.workspace_hash, 'w1');
  assert.match(p.id, /^question-s1-/);
});

run('buildNotificationPayload completion 默认 title suffix 是 "会话"', () => {
  const p = buildNotificationPayload({
    type: 'completion',
    sessionId: 's2',
    bodyText: '任务完成',
  });
  assert.equal(p.title, '已完成 · 会话');
  assert.match(p.id, /^completion-s2-/);
});

run('buildNotificationPayload 空 body 时 completion 走默认占位', () => {
  const p = buildNotificationPayload({ type: 'completion', sessionId: 's3', bodyText: '' });
  assert.equal(p.body, '(空白回合)');
});

run('buildNotificationPayload 空 body 时 question 留空 body', () => {
  const p = buildNotificationPayload({ type: 'question', sessionId: 's4', bodyText: '   ' });
  assert.equal(p.body, '');
});

run('buildNotificationPayload 长 body 被截断', () => {
  const p = buildNotificationPayload({
    type: 'question', sessionId: 's5', bodyText: '一'.repeat(200),
  });
  assert.equal(Array.from(p.body).length, 81);
});

run('buildNotificationPayload 未知 type 当 question', () => {
  const p = buildNotificationPayload({ type: 'weird', sessionId: 's6' });
  assert.match(p.id, /^question-/);
  assert.match(p.title, /^需要你回答/);
});

// shouldSuppress 抑制规则
const sampleQuestion = {
  id: 'question-s1-123',
  session_id: 's1',
  workspace_hash: 'w1',
  title: 't', body: 'b',
};
const sampleCompletion = {
  id: 'completion-s2-123',
  session_id: 's2',
  workspace_hash: 'w1',
  title: 't', body: 'b',
};

run('shouldSuppress: enabled=false 一律抑制', () => {
  assert.equal(shouldSuppress(sampleQuestion, null, false, { enabled: false }), true);
  assert.equal(shouldSuppress(sampleCompletion, null, false, { enabled: false }), true);
});

run('shouldSuppress: 默认 cfg + 窗口失焦 → 不抑制', () => {
  assert.equal(shouldSuppress(sampleQuestion, { sessionId: 's1', workspaceHash: 'w1' }, false, null), false);
});

run('shouldSuppress: on_question=false 抑制 question 但不抑制 completion', () => {
  const cfg = { on_question: false };
  assert.equal(shouldSuppress(sampleQuestion, null, false, cfg), true);
  assert.equal(shouldSuppress(sampleCompletion, null, false, cfg), false);
});

run('shouldSuppress: on_completion=false 抑制 completion 但不抑制 question', () => {
  const cfg = { on_completion: false };
  assert.equal(shouldSuppress(sampleQuestion, null, false, cfg), false);
  assert.equal(shouldSuppress(sampleCompletion, null, false, cfg), true);
});

run('shouldSuppress: 窗口聚焦 + 同 session + suppress_when_focused 默认 → 抑制', () => {
  const active = { sessionId: 's1', workspaceHash: 'w1' };
  assert.equal(shouldSuppress(sampleQuestion, active, true, null), true);
});

run('shouldSuppress: 窗口聚焦但不同 session → 不抑制', () => {
  const active = { sessionId: 'other', workspaceHash: 'w1' };
  assert.equal(shouldSuppress(sampleQuestion, active, true, null), false);
});

run('shouldSuppress: suppress_when_focused=false 即使聚焦也不抑制', () => {
  const active = { sessionId: 's1', workspaceHash: 'w1' };
  const cfg = { suppress_when_focused: false };
  assert.equal(shouldSuppress(sampleQuestion, active, true, cfg), false);
});

run('shouldSuppress: 同 session 但 workspace 不同 → 不抑制', () => {
  const active = { sessionId: 's1', workspaceHash: 'other-ws' };
  assert.equal(shouldSuppress(sampleQuestion, active, true, null), false);
});

// maybeNotify 在无桥时静默 no-op
run('maybeNotify 无 desktop 桥时返回 false 不抛错', () => {
  // 模拟浏览器直访模式:window 无 aceDesktop_notify
  const prev = global.window;
  global.window = {};
  const ok = maybeNotify({
    type: 'question',
    sessionId: 's1',
    bodyText: 'hello',
    activeRef: null,
    hasFocus: false,
    cfg: null,
  });
  assert.equal(ok, false);
  global.window = prev;
});

run('maybeNotify 桥可用 + 抑制规则不命中 → 投递', () => {
  const prev = global.window;
  let captured = null;
  global.window = {
    aceDesktop_notify: (json) => { captured = JSON.parse(json); },
  };
  const ok = maybeNotify({
    type: 'completion',
    sessionId: 's-x',
    workspaceHash: 'w-x',
    sessionTitle: 'Test',
    bodyText: '完工了',
    activeRef: { sessionId: 'other', workspaceHash: 'other-w' }, // 不同 session
    hasFocus: true,
    cfg: null,
  });
  assert.equal(ok, true);
  assert.equal(captured.session_id, 's-x');
  assert.equal(captured.title, '已完成 · Test');
  assert.equal(captured.body, '完工了');
  global.window = prev;
});

run('maybeNotify 桥可用 + 抑制规则命中 → 不投递', () => {
  const prev = global.window;
  let called = 0;
  global.window = {
    aceDesktop_notify: () => { called += 1; },
  };
  const ok = maybeNotify({
    type: 'question',
    sessionId: 's1',
    workspaceHash: 'w1',
    bodyText: 'hi',
    activeRef: { sessionId: 's1', workspaceHash: 'w1' }, // 同 session
    hasFocus: true,
    cfg: null,
  });
  assert.equal(ok, false);
  assert.equal(called, 0);
  global.window = prev;
});
