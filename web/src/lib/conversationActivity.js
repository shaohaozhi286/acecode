export const CONVERSATION_ACTIVITY_KIND = Object.freeze({
  IDLE: 'idle',
  PERMISSION: 'permission',
  QUESTION: 'question',
  RECOVERY: 'recovery',
  FOREGROUND: 'foreground',
  BACKGROUND: 'background',
});

const FALLBACK_PHASE_LABELS = Object.freeze({
  model_waiting: '正在等待模型响应',
  followup_model: '正在继续处理',
  reasoning: '正在分析',
  tool_planning: '正在准备工具调用',
  tool_running: '正在执行工具',
});

function requestIsUnresolved(entry) {
  if (!entry || entry.has_request === false) return false;
  return entry.status !== 'resolved';
}

function requestStartedAtMs(entry) {
  return Number(
    entry?.started_at_ms
      || entry?.timestamp_ms
      || entry?.created_at_ms
      || 0,
  ) || 0;
}

function runningBackgroundTasks(tasks) {
  return (Array.isArray(tasks) ? tasks : []).filter((task) => (
    task?.status === 'running' || task?.busy === true
  ));
}

function oldestBackgroundStart(tasks) {
  let oldest = 0;
  for (const task of tasks) {
    const started = Number(task?.createdAtMs || task?.started_at_ms || 0) || 0;
    if (started && (!oldest || started < oldest)) oldest = started;
  }
  return oldest;
}

function backgroundContext(tasks) {
  const count = tasks.length;
  return {
    backgroundCount: count,
    backgroundLabel: count > 0 ? `${count} 个后台任务正在运行` : '',
  };
}

export function selectConversationActivity({
  foregroundBusy = false,
  foregroundActivity = null,
  permissionRequests = [],
  questionRequest = null,
  subagentTasks = [],
} = {}) {
  const runningTasks = runningBackgroundTasks(subagentTasks);
  const background = backgroundContext(runningTasks);
  const permission = (Array.isArray(permissionRequests) ? permissionRequests : [])
    .find(requestIsUnresolved);

  if (permission) {
    return {
      kind: CONVERSATION_ACTIVITY_KIND.PERMISSION,
      phase: 'permission_waiting',
      label: '等待权限确认',
      detail: permission.origin_label || '',
      startedAtMs: requestStartedAtMs(permission)
        || Number(foregroundActivity?.startedAtMs || 0),
      needsAction: true,
      ...background,
    };
  }

  if (requestIsUnresolved(questionRequest)) {
    return {
      kind: CONVERSATION_ACTIVITY_KIND.QUESTION,
      phase: 'question_waiting',
      label: '等待你回答',
      detail: questionRequest.origin_label || '',
      startedAtMs: requestStartedAtMs(questionRequest)
        || Number(foregroundActivity?.startedAtMs || 0),
      needsAction: true,
      ...background,
    };
  }

  const phase = String(foregroundActivity?.phase || '');
  if (foregroundBusy && (phase === 'permission_waiting' || phase === 'question_waiting')) {
    return {
      kind: CONVERSATION_ACTIVITY_KIND.RECOVERY,
      phase,
      label: phase === 'permission_waiting'
        ? '正在恢复权限请求'
        : '正在恢复提问请求',
      detail: '等待交互内容同步',
      startedAtMs: Number(foregroundActivity?.startedAtMs || 0),
      needsAction: false,
      ...background,
    };
  }

  if (foregroundBusy) {
    return {
      kind: CONVERSATION_ACTIVITY_KIND.FOREGROUND,
      phase: phase || 'working',
      label: foregroundActivity?.label
        || FALLBACK_PHASE_LABELS[phase]
        || '正在处理请求',
      detail: foregroundActivity?.detail || '',
      startedAtMs: Number(foregroundActivity?.startedAtMs || 0),
      needsAction: false,
      ...background,
    };
  }

  if (runningTasks.length > 0) {
    return {
      kind: CONVERSATION_ACTIVITY_KIND.BACKGROUND,
      phase: 'background_tasks',
      label: background.backgroundLabel,
      detail: '主会话仍可继续输入',
      startedAtMs: oldestBackgroundStart(runningTasks),
      needsAction: false,
      ...background,
    };
  }

  return {
    kind: CONVERSATION_ACTIVITY_KIND.IDLE,
    phase: 'idle',
    label: '',
    detail: '',
    startedAtMs: 0,
    needsAction: false,
    ...background,
  };
}
