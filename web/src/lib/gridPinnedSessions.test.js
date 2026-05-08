// 覆盖 web/src/lib/gridPinnedSessions.js 两个纯函数:
//   - pickPinnedSessionsForGrid: Grid4 / Grid9 用来从 listWorkspaceSessions
//     的全量结果里、按当前 workspace 的 pinned IDs 顺序、最多 limit 条提取
//     真正要在宫格里渲染的会话。
//   - sessionRefFromGridPayload: Grid 点击 → ExpandedOverlay → ChatView
//     的 sessionRef 桥,把 server 的 snake_case payload 转成 ChatView 期望
//     的 camelCase 形态;历史 bug 是 ExpandedOverlay 直接读 camelCase,
//     server 给的是 snake_case,workspaceHash 永远 undefined → 下游
//     `/api/workspaces/undefined/...` 404。
//
// 见 openspec/changes/fix-desktop-grid-show-pinned-only。

import assert from 'node:assert/strict';
import {
  pickPinnedSessionsForGrid,
  sessionRefFromGridPayload,
} from './gridPinnedSessions.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// ────────────────────────────────────────────────────────────
// pickPinnedSessionsForGrid
// ────────────────────────────────────────────────────────────

// 触发场景:用户没有 pin 任何会话(空 pinnedIds)。
// 期望行为:返回空数组,Grid 组件层会据此渲染"请在 sidebar 置顶"提示。
test('pickPinnedSessionsForGrid: 0 pinned 返回空', () => {
  const sessions = [{ id: 'a' }, { id: 'b' }];
  assert.deepEqual(pickPinnedSessionsForGrid(sessions, [], 4), []);
});

// 触发场景:sessions 列表完全为空(daemon 刚启,workspace 还没 session)。
// 期望行为:即便 pinnedIds 不空,也只能返回空(没东西可指)。
test('pickPinnedSessionsForGrid: 0 sessions 返回空', () => {
  assert.deepEqual(pickPinnedSessionsForGrid([], ['a', 'b'], 4), []);
});

// 触发场景:sessions 含 pin 与非 pin 混合,pinnedIds 列出 2 个 pin。
// 期望行为:只返回 2 个 pin,且按 pinnedIds 顺序(先 b 后 a),
// 非 pin 的 c / d 永不出现在结果中。
test('pickPinnedSessionsForGrid: 只返回 pin,按 pinnedIds 顺序', () => {
  const sessions = [
    { id: 'a', title: 'A' },
    { id: 'b', title: 'B' },
    { id: 'c', title: 'C' },
    { id: 'd', title: 'D' },
  ];
  const result = pickPinnedSessionsForGrid(sessions, ['b', 'a'], 4);
  assert.equal(result.length, 2);
  assert.equal(result[0].id, 'b');
  assert.equal(result[1].id, 'a');
});

// 触发场景:pinnedIds 长度 > limit(用户置顶了 6 个,Grid4 只能放 4 个)。
// 期望行为:截到前 4 个,且仍按 pinnedIds 顺序;后 2 个不出现。
test('pickPinnedSessionsForGrid: 多于 limit 时按顺序截断', () => {
  const sessions = ['a', 'b', 'c', 'd', 'e', 'f'].map((id) => ({ id }));
  const result = pickPinnedSessionsForGrid(
    sessions,
    ['a', 'b', 'c', 'd', 'e', 'f'],
    4,
  );
  assert.deepEqual(result.map((s) => s.id), ['a', 'b', 'c', 'd']);
});

// 触发场景:pinnedIds 中包含已删除会话的 stale id(对应 spec 里
// "Pinned id pointing to deleted session is skipped")。
// 期望行为:跳过 stale id,继续把后面的有效 pin 填上,总数还是按 limit 算。
test('pickPinnedSessionsForGrid: 跳过指向已删除会话的 stale id', () => {
  const sessions = [{ id: 'a' }, { id: 'c' }];
  // 'b' 在 pinnedIds 里但 sessions 里没有 → 应该被跳过。
  const result = pickPinnedSessionsForGrid(sessions, ['a', 'b', 'c'], 4);
  assert.deepEqual(result.map((s) => s.id), ['a', 'c']);
});

// 触发场景:pinnedIds 含重复(理论上 normalizePinnedIds 已去重,但
// 防御性处理避免上游 bug 让同一 session 在宫格里重复出现)。
// 期望行为:重复 id 只算一次,占一个格子。
test('pickPinnedSessionsForGrid: pinnedIds 重复 id 去重', () => {
  const sessions = [{ id: 'a' }, { id: 'b' }];
  const result = pickPinnedSessionsForGrid(sessions, ['a', 'a', 'b', 'a'], 4);
  assert.deepEqual(result.map((s) => s.id), ['a', 'b']);
});

// 触发场景:sessions 用了不同的 id 字段名(snake_case session_id 或
// camelCase sessionId 都可能出现 — server 用 id,但有些 client 路径用 session_id)。
// 期望行为:都能命中,不挑命名。
test('pickPinnedSessionsForGrid: 兼容 id / session_id / sessionId 三种命名', () => {
  const sessions = [
    { session_id: 'x', title: 'X by session_id' },
    { sessionId: 'y', title: 'Y by sessionId' },
    { id: 'z', title: 'Z by id' },
  ];
  const result = pickPinnedSessionsForGrid(sessions, ['x', 'y', 'z'], 4);
  assert.equal(result.length, 3);
  assert.equal(result[0].title, 'X by session_id');
  assert.equal(result[1].title, 'Y by sessionId');
  assert.equal(result[2].title, 'Z by id');
});

// 触发场景:limit 给非法值(0 / 负数 / NaN / undefined)。
// 期望行为:统一返回空数组,不抛异常,组件层不需要再守 NaN。
test('pickPinnedSessionsForGrid: 非法 limit 一律返回空', () => {
  const sessions = [{ id: 'a' }];
  const pinnedIds = ['a'];
  assert.deepEqual(pickPinnedSessionsForGrid(sessions, pinnedIds, 0), []);
  assert.deepEqual(pickPinnedSessionsForGrid(sessions, pinnedIds, -1), []);
  assert.deepEqual(pickPinnedSessionsForGrid(sessions, pinnedIds, NaN), []);
  assert.deepEqual(pickPinnedSessionsForGrid(sessions, pinnedIds), []);
});

// 触发场景:输入非数组(null / undefined / 字符串等坏值,典型 race
// 时 listWorkspaceSessions 还没回来)。
// 期望行为:返回空数组,不抛。
test('pickPinnedSessionsForGrid: 非数组输入安全降级', () => {
  assert.deepEqual(pickPinnedSessionsForGrid(null, ['a'], 4), []);
  assert.deepEqual(pickPinnedSessionsForGrid([{ id: 'a' }], null, 4), []);
  assert.deepEqual(pickPinnedSessionsForGrid('not-array', 'also-not', 4), []);
});

// ────────────────────────────────────────────────────────────
// sessionRefFromGridPayload (回归测试:见 openspec change Why 段)
// ────────────────────────────────────────────────────────────

// 触发场景(回归测试,2026-05-08 实际 bug):server 的
// session_info_to_json / session_meta_to_json 给的 payload 全是
// snake_case 字段(workspace_hash / display_title / message_count),
// 没有 camelCase alias。
// 期望行为:输出的 sessionRef 里 workspaceHash 非空、且等于 server 的
// workspace_hash;displayTitle / message_count 等其它字段同样从 snake_case
// 来源正确填充;没有任何关键字段为 undefined。
// 修复前 bug 表现:ExpandedOverlay 用 `session.workspaceHash`(camelCase)
// 直接读,值是 undefined,下游 `/api/workspaces/${undefined}/...` 触发
// HTTP 404 workspace not found。
test('sessionRefFromGridPayload: server snake_case payload 正确填充 camelCase', () => {
  const payload = {
    id: '20260506-160124-b0c0',
    workspace_hash: '5ff8131c91b2e46c',
    cwd: 'N:\\Users\\shao\\dbcoding2',
    active: true,
    busy: false,
    status: 'idle',
    title: '测试会话',
    summary: '',
    display_title: '新会话1',
    message_count: 12,
    created_at: '2026-05-06T16:01:24Z',
    updated_at: '2026-05-08T14:00:00Z',
    provider: 'openai',
    model: 'glm-5.1',
  };
  const ref = sessionRefFromGridPayload(payload);

  assert.ok(ref, 'ref must not be null');
  assert.equal(ref.sessionId, '20260506-160124-b0c0');
  // 关键回归点:workspaceHash 必须非空 = server 的 workspace_hash。
  assert.equal(ref.workspaceHash, '5ff8131c91b2e46c');
  assert.notEqual(ref.workspaceHash, '');
  assert.notEqual(ref.workspaceHash, undefined);
  assert.equal(ref.cwd, 'N:\\Users\\shao\\dbcoding2');
  assert.equal(ref.active, true);
  assert.equal(ref.busy, false);
  assert.equal(ref.status, 'idle');
  assert.equal(ref.title, '测试会话');
  assert.equal(ref.displayTitle, '新会话1');
  assert.equal(ref.message_count, 12);
  assert.equal(ref.created_at, '2026-05-06T16:01:24Z');
  assert.equal(ref.updated_at, '2026-05-08T14:00:00Z');
});

// 触发场景:payload 用 camelCase(MiniSession 已规范化过的对象)。
// 期望行为:读 camelCase 不出错,跟 snake_case 路径等价。
test('sessionRefFromGridPayload: camelCase payload 也能正常工作', () => {
  const payload = {
    sessionId: 'abc-123',
    workspaceHash: 'hashv1',
    displayTitle: 'd1',
  };
  const ref = sessionRefFromGridPayload(payload);
  assert.equal(ref.sessionId, 'abc-123');
  assert.equal(ref.workspaceHash, 'hashv1');
  assert.equal(ref.displayTitle, 'd1');
});

// 触发场景:payload 同时给 camelCase 与 snake_case(理论上不会发生,
// 但同一对象经过多个规范化层后可能两者都在)。
// 期望行为:camelCase 优先(它通常是已规范化后的值)。
test('sessionRefFromGridPayload: 两种命名同时存在时 camelCase 优先', () => {
  const payload = {
    id: 'x',
    workspaceHash: 'camel',
    workspace_hash: 'snake',
    displayTitle: 'camel-title',
    display_title: 'snake-title',
  };
  const ref = sessionRefFromGridPayload(payload);
  assert.equal(ref.workspaceHash, 'camel');
  assert.equal(ref.displayTitle, 'camel-title');
});

// 触发场景:坏输入(null / 没有 id / 不是对象)。
// 期望行为:返回 null,组件层据此跳过 ExpandedOverlay 打开;不抛。
test('sessionRefFromGridPayload: 坏输入返回 null', () => {
  assert.equal(sessionRefFromGridPayload(null), null);
  assert.equal(sessionRefFromGridPayload(undefined), null);
  assert.equal(sessionRefFromGridPayload('string'), null);
  assert.equal(sessionRefFromGridPayload({}), null);  // 没 id
  assert.equal(sessionRefFromGridPayload({ workspace_hash: 'x' }), null);  // 没 id
});

// 触发场景:payload 给的 message_count 是字符串 / 缺失。
// 期望行为:转成 number(0 兜底),不让下游 NaN 比较失败。
test('sessionRefFromGridPayload: message_count 类型规范化', () => {
  const ref1 = sessionRefFromGridPayload({ id: 'x' });
  assert.equal(ref1.message_count, 0);

  const ref2 = sessionRefFromGridPayload({ id: 'x', message_count: '42' });
  assert.equal(ref2.message_count, 42);

  const ref3 = sessionRefFromGridPayload({ id: 'x', messageCount: 7 });
  assert.equal(ref3.message_count, 7);
});

// 触发场景:server 缺 workspace_hash(极少见,但 daemon 在 compatibility
// 模式下可能不带 workspace 信息)。
// 期望行为:workspaceHash 是空字符串(不是 undefined),下游 `||` 兜底
// 时仍能走 fallbackWorkspaceOption 路径,不至于把 undefined 拼进 URL。
test('sessionRefFromGridPayload: 缺 workspace_hash 时给空字符串而非 undefined', () => {
  const ref = sessionRefFromGridPayload({ id: 'x' });
  assert.equal(ref.workspaceHash, '');
  assert.notEqual(ref.workspaceHash, undefined);
});
