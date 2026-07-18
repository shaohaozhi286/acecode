// 后台任务(spawn_subagent 子会话)面板的纯状态逻辑。
//
// 数据来源分两层:
//   1. REST GET /api/sessions?parent=<id> 的会话快照(标题/tokens/时间戳)
//      → normalizeSubagentTask 归一化成任务对象。
//   2. 子会话的实时 WS 事件(busy_changed / usage / tool_start / tool_end /
//      session_updated)→ applySubagentSessionEvent 增量更新运行中卡片的
//      统计(耗时基点、tokens、工具计数、当前工具)。
//
// DOM 端(SubagentPanel.jsx)只消费这里产出的结构;所有状态转移在此测试。

export const SUBAGENT_TASK_STATUS = Object.freeze({
  RUNNING: 'running',
  COMPLETED: 'completed',
  ABORTED: 'aborted',
});

function readTokens(session) {
  const usage = session?.session_token_usage || session?.sessionTokenUsage || null;
  const total = Number(usage?.total_tokens ?? usage?.totalTokens);
  return Number.isFinite(total) && total > 0 ? total : 0;
}

function parseIsoMs(value) {
  if (!value) return 0;
  const ms = Date.parse(String(value));
  return Number.isFinite(ms) ? ms : 0;
}

// REST 会话快照 → 任务对象。busy(active 会话实时)或 status==='running'
// 视为运行中;其余是已结束(区分不了"完成/中止",统一 completed,中止由
// 前端 abort 操作时本地标记)。
export function normalizeSubagentTask(session) {
  if (!session || typeof session !== 'object') return null;
  const id = String(session.id || session.session_id || '');
  if (!id) return null;
  const busy = session.busy === true || session.status === 'running';
  return {
    id,
    parentId: String(session.parent_session_id || ''),
    title: String(session.title || '').trim(),
    summary: String(session.summary || '').trim(),
    status: busy ? SUBAGENT_TASK_STATUS.RUNNING : SUBAGENT_TASK_STATUS.COMPLETED,
    createdAtMs: parseIsoMs(session.created_at),
    updatedAtMs: parseIsoMs(session.updated_at),
    tokens: readTokens(session),
    turnCount: Math.max(0, Number(session.turn_count) || 0),
    // 实时聚合字段:REST 快照没有工具级数据,从 0 起由 WS 事件累积。
    toolCount: 0,
    lastTool: '',
    model: String(session.model || ''),
  };
}

// REST 列表 → 任务数组(按创建时间倒序,新任务在前)。保留旧任务的实时
// 聚合字段(toolCount/lastTool)与本地 aborted 标记,避免 refetch 清零。
export function mergeSubagentTaskList(prevTasks, sessions) {
  const prevById = new Map((prevTasks || []).map((t) => [t.id, t]));
  const next = [];
  for (const session of Array.isArray(sessions) ? sessions : []) {
    const task = normalizeSubagentTask(session);
    if (!task) continue;
    const prev = prevById.get(task.id);
    if (prev) {
      task.toolCount = prev.toolCount;
      task.lastTool = prev.lastTool;
      if (prev.status === SUBAGENT_TASK_STATUS.ABORTED &&
          task.status !== SUBAGENT_TASK_STATUS.RUNNING) {
        task.status = SUBAGENT_TASK_STATUS.ABORTED;
      }
    }
    next.push(task);
  }
  next.sort((a, b) => (b.createdAtMs - a.createdAtMs) || (a.id < b.id ? 1 : -1));
  return next;
}

// Parent subscriptions receive additive child session_status events. Only an
// explicit parent match may trigger discovery: treating every unknown busy
// workspace session as a child causes unrelated conversations to refetch and
// can attach permission/question attention to the wrong parent.
export function shouldRefreshSubagentTasksFromStatus(
  parentSessionId,
  knownTaskIds,
  msg,
) {
  if (!parentSessionId || msg?.type !== 'session_status') return false;
  const payload = msg?.payload || {};
  const sessionId = String(payload.session_id || msg?.session_id || '').trim();
  const statusParentId = String(
    payload.parent_session_id || msg?.parent_session_id || '',
  ).trim();
  if (!sessionId || statusParentId !== parentSessionId) return false;
  return !(knownTaskIds instanceof Set && knownTaskIds.has(sessionId));
}

// 子会话自己的 WS 事件 → 任务增量。返回新数组;无关事件返回原引用
// (调用方可用引用相等跳过 setState)。
export function applySubagentSessionEvent(tasks, msg) {
  const sid = msg?.session_id || msg?.payload?.session_id || '';
  if (!sid) return tasks;
  const index = (tasks || []).findIndex((t) => t.id === sid);
  if (index < 0) return tasks;
  const task = tasks[index];
  const type = msg?.type || '';
  const p = msg?.payload || {};
  let patch = null;

  if (type === 'busy_changed' || type === 'session_status') {
    const busy = p.busy === true;
    if (busy && task.status !== SUBAGENT_TASK_STATUS.RUNNING) {
      patch = { status: SUBAGENT_TASK_STATUS.RUNNING };
    } else if (!busy && task.status === SUBAGENT_TASK_STATUS.RUNNING) {
      patch = { status: SUBAGENT_TASK_STATUS.COMPLETED, updatedAtMs: Date.now() };
    }
  } else if (type === 'usage') {
    const total = Number(p.total_tokens ?? p.totalTokens);
    if (Number.isFinite(total) && total > task.tokens) {
      patch = { tokens: total };
    }
  } else if (type === 'tool_start') {
    patch = {
      toolCount: task.toolCount + 1,
      lastTool: String(p.tool || task.lastTool || ''),
    };
  } else if (type === 'tool_end') {
    const tool = String(p.tool || '');
    if (tool && tool !== task.lastTool) patch = { lastTool: tool };
  } else if (type === 'session_updated') {
    const title = String(p.title || '').trim();
    if (title && title !== task.title) patch = { title };
  }

  if (!patch) return tasks;
  const next = tasks.slice();
  next[index] = { ...task, ...patch };
  return next;
}

// 本地中止标记:sendAbort 之后立即把运行中任务显示为「已中止」,不等
// busy_changed 往返(之后 busy_changed(false) 到达时 status 已非 RUNNING,
// applySubagentSessionEvent 不会覆盖回 completed)。
export function markSubagentTaskAborted(tasks, id) {
  const index = (tasks || []).findIndex((t) => t.id === id);
  if (index < 0) return tasks;
  const next = tasks.slice();
  next[index] = { ...next[index], status: SUBAGENT_TASK_STATUS.ABORTED, updatedAtMs: Date.now() };
  return next;
}

export function removeSubagentTask(tasks, id) {
  const next = (tasks || []).filter((t) => t.id !== id);
  return next.length === (tasks || []).length ? tasks : next;
}

export function subagentTaskGroups(tasks) {
  const running = [];
  const settled = [];
  for (const task of tasks || []) {
    (task.status === SUBAGENT_TASK_STATUS.RUNNING ? running : settled).push(task);
  }
  return { running, settled };
}

export function runningSubagentCount(tasks) {
  return (tasks || []).reduce(
    (n, t) => n + (t.status === SUBAGENT_TASK_STATUS.RUNNING ? 1 : 0), 0);
}

// '34.0k' 风格的 token 数(图中卡片样式);< 1000 原样。
export function formatTaskTokens(total) {
  const n = Math.max(0, Number(total) || 0);
  if (n < 1000) return String(n);
  const k = n / 1000;
  return `${k >= 100 ? Math.round(k) : k.toFixed(1)}k`;
}

// 卡片耗时:运行中 = now - createdAt;已结束 = updatedAt - createdAt。
export function taskElapsedSeconds(task, nowMs = Date.now()) {
  if (!task?.createdAtMs) return 0;
  const end = task.status === SUBAGENT_TASK_STATUS.RUNNING
    ? nowMs
    : (task.updatedAtMs || nowMs);
  return Math.max(0, Math.round((end - task.createdAtMs) / 1000));
}

export function formatElapsed(seconds) {
  const s = Math.max(0, Math.round(Number(seconds) || 0));
  if (s < 60) return `${String(s).padStart(2, '0')}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m${String(s % 60).padStart(2, '0')}s`;
  const h = Math.floor(m / 60);
  return `${h}h${String(m % 60).padStart(2, '0')}m`;
}

// 卡片统计行:'34.0k tokens · 2 tools · Bash' 的分段(空段过滤)。
export function taskStatsParts(task) {
  const parts = [];
  if (task?.tokens > 0) parts.push(`${formatTaskTokens(task.tokens)} tokens`);
  if (task?.toolCount > 0) parts.push(`${task.toolCount} ${task.toolCount === 1 ? 'tool' : 'tools'}`);
  if (task?.lastTool) parts.push(task.lastTool);
  return parts;
}

// 卡片标题:auto-title 优先,退到首条 user 输入摘要,再退会话 id。
// auto-title 生成失败会留下 "[Error] ..." 形式的占位(后端
// is_generated_error_title 语义)——对用户无意义,直接退到摘要。
export function taskDisplayTitle(task) {
  const title = String(task?.title || '');
  if (title && !title.startsWith('[Error]')) return title;
  return task?.summary || task?.id || '';
}

// 任务状态 → 中文标签(已结束组用)。
export function taskStatusLabel(task) {
  if (task?.status === SUBAGENT_TASK_STATUS.RUNNING) return '运行中';
  if (task?.status === SUBAGENT_TASK_STATUS.ABORTED) return '已中止';
  return 'Completed';
}
