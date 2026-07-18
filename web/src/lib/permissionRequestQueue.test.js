import assert from 'node:assert/strict';
import {
  PERMISSION_REQUEST_STATUS,
  clearResolvedPermissionRequests,
  closePermissionRequest,
  hasUnresolvedPermission,
  markPermissionSubmitting,
  pendingPermissionSessionIds,
  permissionOriginLabel,
  pushPermissionRequest,
  sessionHasPendingPermission,
  visiblePermissionRequests,
} from './permissionRequestQueue.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const reqA = {
  request_id: 'rid-a',
  session_id: 'session-a',
  tool: 'file_write',
  args: { file_path: 'a.go' },
};
const reqB = {
  request_id: 'rid-b',
  session_id: 'session-a',
  tool: 'bash',
  args: { command: 'pnpm test' },
};

run('request 生命周期 pending -> submitting -> resolved', () => {
  let list = pushPermissionRequest([], reqA);
  assert.equal(list[0].status, PERMISSION_REQUEST_STATUS.PENDING);
  list = markPermissionSubmitting(list, 'rid-a', 'allow');
  assert.equal(list[0].status, PERMISSION_REQUEST_STATUS.SUBMITTING);
  assert.equal(list[0].submitted_choice, 'allow');
  list = closePermissionRequest(list, {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'allow',
    reason: 'decision',
  });
  assert.equal(list[0].status, PERMISSION_REQUEST_STATUS.RESOLVED);
  assert.equal(list[0].choice, 'allow');
  assert.equal(list[0].reason, 'decision');
  assert.equal(hasUnresolvedPermission(list), false);
});

run('重复 request 不重复入队并保持 submitting 状态', () => {
  let list = pushPermissionRequest([], reqA);
  list = markPermissionSubmitting(list, 'rid-a', 'deny');
  list = pushPermissionRequest(list, { ...reqA, args: { file_path: 'a.go' } });
  assert.equal(list.length, 1);
  assert.equal(list[0].status, PERMISSION_REQUEST_STATUS.SUBMITTING);
  assert.equal(list[0].submitted_choice, 'deny');
});

run('close 先到时保留 tombstone,后到 request 只补展示数据且不复活', () => {
  let list = closePermissionRequest([], {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'deny',
    reason: 'abort',
  });
  assert.equal(list[0].has_request, false);
  list = pushPermissionRequest(list, reqA);
  assert.equal(list.length, 1);
  assert.equal(list[0].has_request, true);
  assert.equal(list[0].status, PERMISSION_REQUEST_STATUS.RESOLVED);
  assert.equal(list[0].choice, 'deny');
  assert.equal(list[0].reason, 'abort');
});

run('重复 close 幂等且不会改变首次服务端结论', () => {
  let list = pushPermissionRequest([], reqA);
  list = closePermissionRequest(list, {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'allow',
    reason: 'decision',
  });
  const first = list;
  list = closePermissionRequest(list, {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'deny',
    reason: 'timeout',
  });
  assert.equal(list, first);
  assert.equal(list[0].choice, 'allow');
});

run('reconnect request replay + pending snapshot + close replay 全程不重复或复活', () => {
  let list = pushPermissionRequest([], reqA); // sequenced request replay
  list = pushPermissionRequest(list, { ...reqA }); // seq-less pending snapshot
  assert.equal(list.length, 1);
  list = closePermissionRequest(list, {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'allow_session',
    reason: 'decision',
  });
  list = closePermissionRequest(list, { // duplicate close replay
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'allow_session',
    reason: 'decision',
  });
  list = pushPermissionRequest(list, { ...reqA }); // delayed duplicate snapshot
  assert.equal(list.length, 1);
  assert.equal(list[0].status, PERMISSION_REQUEST_STATUS.RESOLVED);
  assert.equal(list[0].choice, 'allow_session');
});

run('multi-client:一端决策后的 server close 会让另一端立即只读', () => {
  let clientA = pushPermissionRequest([], reqA);
  let clientB = pushPermissionRequest([], reqA);
  clientA = markPermissionSubmitting(clientA, 'rid-a', 'deny');
  assert.equal(clientA[0].status, PERMISSION_REQUEST_STATUS.SUBMITTING);
  assert.equal(clientB[0].status, PERMISSION_REQUEST_STATUS.PENDING);

  const close = {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'deny',
    reason: 'decision',
  };
  clientA = closePermissionRequest(clientA, close);
  clientB = closePermissionRequest(clientB, close);
  assert.equal(clientA[0].status, PERMISSION_REQUEST_STATUS.RESOLVED);
  assert.equal(clientB[0].status, PERMISSION_REQUEST_STATUS.RESOLVED);
  assert.equal(hasUnresolvedPermission(clientB), false);
});

run('同会话 FIFO:只暴露第一条未决请求,已解决卡可与下一条并存', () => {
  let list = pushPermissionRequest([], reqA);
  list = pushPermissionRequest(list, reqB);
  let visible = visiblePermissionRequests(list, 'session-a');
  assert.deepEqual(visible.map((entry) => entry.request_id), ['rid-a']);

  list = closePermissionRequest(list, {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'allow',
    reason: 'decision',
  });
  visible = visiblePermissionRequests(list, 'session-a');
  assert.deepEqual(visible.map((entry) => entry.request_id), ['rid-a', 'rid-b']);
});

run('其它会话请求不会进入当前会话或压制当前会话交互', () => {
  const list = pushPermissionRequest([], reqA);
  assert.deepEqual(visiblePermissionRequests(list, 'session-b'), []);
  assert.equal(hasUnresolvedPermission(visiblePermissionRequests(list, 'session-b')), false);
  assert.equal(hasUnresolvedPermission(visiblePermissionRequests(list, 'session-a')), true);
});

run('子代理请求归入父会话,保留来源标题和子 session 路由', () => {
  const ownership = {
    owners: { 'child-a': 'parent-a' },
    titles: { 'child-a': '测试后台任务' },
  };
  const list = pushPermissionRequest([], {
    ...reqA,
    session_id: 'child-a',
  });
  const visible = visiblePermissionRequests(list, 'parent-a', ownership);
  assert.equal(visible.length, 1);
  assert.equal(visible[0].session_id, 'child-a');
  assert.equal(permissionOriginLabel(visible[0], ownership), '来自后台任务:测试后台任务');
  assert.deepEqual([...pendingPermissionSessionIds(list, '', ownership)], ['parent-a']);
});

run('inactive session 集合只包含未决 owner,可供普通与置顶行复用', () => {
  let list = pushPermissionRequest([], reqA);
  list = pushPermissionRequest(list, {
    ...reqB,
    request_id: 'rid-c',
    session_id: 'session-b',
  });
  list = closePermissionRequest(list, {
    request_id: 'rid-c',
    session_id: 'session-b',
    choice: 'deny',
    reason: 'decision',
  });
  const ids = pendingPermissionSessionIds(list);
  assert.deepEqual([...ids], ['session-a']);
  assert.equal(sessionHasPendingPermission({ id: 'session-a' }, ids), true);
  assert.equal(sessionHasPendingPermission({ id: 'session-b' }, ids), false);
});

run('turn terminal 只清除该实际 session 的 resolved 卡和 tombstone', () => {
  let list = closePermissionRequest([], {
    request_id: 'rid-a',
    session_id: 'session-a',
    choice: 'deny',
    reason: 'timeout',
  });
  list = closePermissionRequest(list, {
    request_id: 'rid-b',
    session_id: 'session-b',
    choice: 'deny',
    reason: 'abort',
  });
  list = clearResolvedPermissionRequests(list, 'session-a');
  assert.deepEqual(list.map((entry) => entry.request_id), ['rid-b']);
});

run('缺 request_id 的 request/close/submitting 都是 no-op', () => {
  const list = [];
  assert.equal(pushPermissionRequest(list, { tool: 'bash' }), list);
  assert.equal(closePermissionRequest(list, { choice: 'deny' }), list);
  assert.equal(markPermissionSubmitting(list, '', 'deny'), list);
});

console.log('permissionRequestQueue tests passed');
