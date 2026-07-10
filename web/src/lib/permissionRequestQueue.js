// 权限请求队列(App.jsx 的 permReqs)的纯状态操作。
//
// 背景:permission_request 经 WS 实时帧 + subscribe 补发(routes_ws.cpp 的
// snapshot_pending_requests)两条通路到达,可能重复 → push 必须按 request_id
// 去重。移除侧曾是"盲删队首 slice(1)":PermissionModal 的 respond 里
// setTimeout(220ms) 与 Modal close 动画(200ms 后 onClose)双重触发 onResolve,
// 一次点击删两条 —— 若"拒绝"后同一批次的下一个写工具在窗口内弹出新请求
// (agent_loop Phase 2 串行写工具,deny 解除阻塞后毫秒级发出下一条),新请求
// 被第二刀误删:弹窗永远不出现,后端 AsyncPrompter 空等 5 分钟超时。
// 修复 = 移除按 request_id 幂等过滤,重复触发无害。

// 入队:按 request_id 去重;缺 request_id 的 payload 丢弃(无法路由决策)。
export function pushPermissionRequest(list, payload) {
  if (!payload?.request_id) return list;
  if (list.some((x) => x.request_id === payload.request_id)) return list;
  return [...list, payload];
}

// 幂等移除:未命中(含 requestId 为空)返回原引用,不触发 React 重渲染。
export function removePermissionRequest(list, requestId) {
  if (!requestId) return list;
  const next = list.filter((x) => x.request_id !== requestId);
  return next.length === list.length ? list : next;
}
