import assert from 'node:assert/strict';
import {
  pushPermissionRequest,
  removePermissionRequest,
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

const reqA = { request_id: 'rid-a', tool: 'file_write', args: { path: 'a.go' } };
const reqB = { request_id: 'rid-b', tool: 'file_write', args: { path: 'b.go' } };

run('push 按 request_id 去重(实时帧与 subscribe 补发重复推送同一请求)', () => {
  // 触发场景:WS 重连后 subscribe 补发 pending 请求,与之前收到的实时帧重复。
  // 期望:同 request_id 只保留一条,且返回原引用避免无谓重渲染。
  let list = [];
  list = pushPermissionRequest(list, reqA);
  const dup = pushPermissionRequest(list, { ...reqA });
  assert.equal(dup, list);
  assert.equal(list.length, 1);
});

run('push 丢弃缺 request_id 的 payload(决策无法路由,不该进队列)', () => {
  const list = [];
  assert.equal(pushPermissionRequest(list, { tool: 'bash' }), list);
  assert.equal(pushPermissionRequest(list, null), list);
});

run('remove 按 request_id 精确移除,不影响其它请求', () => {
  let list = [];
  list = pushPermissionRequest(list, reqA);
  list = pushPermissionRequest(list, reqB);
  list = removePermissionRequest(list, 'rid-a');
  assert.deepEqual(list.map((x) => x.request_id), ['rid-b']);
});

run('回归:重复移除同一 request_id 不误删后续请求', () => {
  // Bug 表现:旧实现 onResolve 盲删队首(slice(1)),而 PermissionModal 的
  // close 动画路径(~200ms)与 respond 自身 setTimeout(220ms)会双重触发
  // onResolve。用户点"拒绝"后,agent_loop 串行执行同批次下一个写工具、
  // 毫秒级发来新请求 B 落进这 220ms 窗口 → 第二刀把 B 删掉,弹窗永远
  // 不出现,后端 AsyncPrompter 空等 5 分钟超时(UI 一直"正在等待权限确认")。
  // 期望:按 request_id 幂等移除,第二次触发是 no-op,B 存活并弹窗。
  let list = [];
  list = pushPermissionRequest(list, reqA);
  list = removePermissionRequest(list, 'rid-a'); // 第一次:close 动画 onClose 路径
  list = pushPermissionRequest(list, reqB);      // 拒绝后下一条请求在窗口内到达
  list = removePermissionRequest(list, 'rid-a'); // 第二次:历史上的 setTimeout 路径
  assert.deepEqual(list.map((x) => x.request_id), ['rid-b']);
});

run('remove 未命中/空 id 返回原引用(no-op 不触发重渲染)', () => {
  const list = [reqA];
  assert.equal(removePermissionRequest(list, 'rid-unknown'), list);
  assert.equal(removePermissionRequest(list, ''), list);
});

console.log('permissionRequestQueue tests passed');
