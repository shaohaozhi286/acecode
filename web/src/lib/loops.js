import { effectiveLocale } from '../i18n/index.js';
import { formatDateTime, formatNumber } from './format.js';

export const LOOP_TEMPLATES = [
  {
    id: 'daily-review',
    name: '每日代码健康巡检',
    description: '检查最近改动、潜在缺陷和缺失测试，并给出可执行建议。',
    prompt: '检查工作区最近的代码改动，找出潜在缺陷、兼容性风险和缺失测试。修复明确且安全的问题，运行相关测试，并总结结果。',
    form: { scheduleKind: 'period', period: 'daily', time: '09:00' },
  },
  {
    id: 'test-fix',
    name: '定时测试与问题修复',
    description: '运行核心测试，定位并修复可可靠复现的问题。',
    prompt: '运行项目的核心测试，调查失败或不稳定用例。修复能够可靠复现且属于项目代码的问题，并报告测试覆盖与遗留风险。',
    form: { scheduleKind: 'interval', intervalValue: 6, intervalUnit: 'hours' },
  },
  {
    id: 'weekly-summary',
    name: '每周代码变更总结',
    description: '汇总代码变化、未完成事项和下一步建议。',
    prompt: '汇总本周工作区中的重要代码变化、已解决问题、仍未完成的事项和技术风险。输出简洁的开发周报与下一步建议，不修改代码。',
    form: { scheduleKind: 'period', period: 'weekly', weekdays: [5], time: '17:00' },
  },
  {
    id: 'debt-audit',
    name: '依赖与技术债巡检',
    description: '检查过期依赖、安全风险和可控范围内的技术债。',
    prompt: '检查项目依赖的可用更新、已知安全风险和范围明确的技术债。只实施低风险修复，验证构建和测试，并说明其余需要人工决策的事项。',
    form: { scheduleKind: 'period', period: 'weekly', weekdays: [1], time: '10:00' },
  },
];

export function defaultLoopForm(modelName = '', now = Date.now()) {
  return {
    name: '',
    prompt: '',
    workspaceHash: '',
    useWorktree: false,
    permissionMode: 'yolo',
    modelName,
    scheduleKind: 'period',
    period: 'daily',
    weekdays: [1],
    time: '09:00',
    intervalValue: 1,
    intervalUnit: 'hours',
    onceAt: toLocalInput(now + 60 * 60 * 1000),
    validFrom: '',
    validUntil: '',
    enabled: true,
  };
}

export function loopFormForTemplate(template, modelName = '', now = Date.now()) {
  const base = defaultLoopForm(modelName, now);
  if (!template) return base;
  return {
    ...base,
    ...(template.form || {}),
    name: template.name || '',
    prompt: template.prompt || '',
  };
}

export function formFromLoop(loop) {
  const schedule = loop?.schedule || {};
  const [hour, minute] = [schedule.hour ?? 9, schedule.minute ?? 0];
  return {
    ...defaultLoopForm(loop?.model_name || ''),
    name: loop?.name || '',
    prompt: loop?.prompt || '',
    workspaceHash: loop?.workspace_hash || '',
    useWorktree: !!loop?.workspace_hash && loop?.use_worktree === true,
    permissionMode: loop?.permission_mode === 'default' ? 'default' : 'yolo',
    modelName: loop?.model_name || '',
    scheduleKind: schedule.kind || 'period',
    period: schedule.period || 'daily',
    weekdays: Array.isArray(schedule.weekdays) ? schedule.weekdays : [1],
    time: `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`,
    intervalValue: schedule.interval_value || 1,
    intervalUnit: schedule.interval_unit || 'hours',
    onceAt: schedule.once_at_ms ? toLocalInput(schedule.once_at_ms) : '',
    validFrom: schedule.valid_from_ms ? toLocalInput(schedule.valid_from_ms) : '',
    validUntil: schedule.valid_until_ms ? toLocalInput(schedule.valid_until_ms) : '',
    enabled: loop?.enabled !== false,
  };
}

export function toLocalInput(timestamp) {
  const date = new Date(timestamp);
  if (!Number.isFinite(date.getTime())) return '';
  const offset = date.getTimezoneOffset() * 60000;
  return new Date(date.getTime() - offset).toISOString().slice(0, 16);
}

function localInputMs(value) {
  if (!value) return null;
  const timestamp = new Date(value).getTime();
  return Number.isFinite(timestamp) ? timestamp : null;
}

export function validateLoopForm(form) {
  if (!String(form?.name || '').trim()) return '请输入循环名称';
  if (!String(form?.prompt || '').trim()) return '请输入提示词';
  if (!String(form?.modelName || '').trim()) return '请选择模型';
  if (form.scheduleKind === 'interval' && Number(form.intervalValue) < 1) return '执行间隔必须大于 0';
  if (form.scheduleKind === 'once' && !localInputMs(form.onceAt)) return '请选择执行时间';
  const from = localInputMs(form.validFrom);
  const until = localInputMs(form.validUntil);
  if (from && until && until <= from) return '结束时间必须晚于开始时间';
  if (form.period === 'weekly' && (!Array.isArray(form.weekdays) || form.weekdays.length === 0)) {
    return '请至少选择一个星期';
  }
  return '';
}

export function buildLoopPayload(form, workspaces = []) {
  const workspace = workspaces.find((item) => item.hash === form.workspaceHash);
  const [hour, minute] = String(form.time || '09:00').split(':').map(Number);
  const schedule = {
    kind: form.scheduleKind,
    valid_from_ms: localInputMs(form.validFrom),
    valid_until_ms: localInputMs(form.validUntil),
  };
  if (form.scheduleKind === 'period') {
    Object.assign(schedule, {
      period: form.period,
      weekdays: form.period === 'weekly' ? [...form.weekdays].sort((a, b) => a - b) : [],
      hour: Number.isFinite(hour) ? hour : 9,
      minute: Number.isFinite(minute) ? minute : 0,
    });
  } else if (form.scheduleKind === 'interval') {
    Object.assign(schedule, {
      interval_value: Number(form.intervalValue),
      interval_unit: form.intervalUnit,
      anchor_ms: Date.now(),
    });
  } else {
    schedule.once_at_ms = localInputMs(form.onceAt);
  }
  return {
    name: String(form.name).trim(),
    prompt: String(form.prompt).trim(),
    workspace_hash: workspace?.hash || '',
    workspace_cwd: workspace?.cwd || '',
    use_worktree: !!workspace && form.useWorktree === true,
    model_name: form.modelName,
    permission_mode: form.permissionMode === 'default' ? 'default' : 'yolo',
    schedule,
    enabled: form.enabled !== false,
  };
}

export function loopScheduleLabel(schedule = {}) {
  const locale = effectiveLocale();
  const time = `${String(schedule.hour ?? 9).padStart(2, '0')}:${String(schedule.minute ?? 0).padStart(2, '0')}`;
  if (schedule.kind === 'once') return `单次 · ${formatDate(schedule.once_at_ms)}`;
  if (schedule.kind === 'interval') {
    const units = { minutes: '分钟', hours: '小时', days: '天' };
    return `每 ${formatNumber(schedule.interval_value || 1, {}, locale)} ${units[schedule.interval_unit] || '小时'}`;
  }
  if (schedule.period === 'workdays') return `工作日 ${time}`;
  if (schedule.period === 'weekly') {
    const labels = Array.from({ length: 7 }, (_, day) =>
      new Intl.DateTimeFormat(locale, { weekday: 'short', timeZone: 'UTC' })
        .format(new Date(Date.UTC(2026, 5, 7 + day))));
    const weekdays = (schedule.weekdays || []).map((day) => labels[day]);
    const list = new Intl.ListFormat(locale, { style: 'short', type: 'conjunction' })
      .format(weekdays);
    return `每周${list} ${time}`;
  }
  return `每天 ${time}`;
}

export function formatDate(value) {
  if (!value) return '—';
  const date = new Date(Number(value));
  if (!Number.isFinite(date.getTime())) return '—';
  return formatDateTime(date, {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

export function loopRunPresentation(run = {}) {
  const labels = {
    scheduled: '等待执行', running: '执行中', waiting_user: '等待用户',
    completed: '已完成', failed: '失败', missed: '已错过',
  };
  const reasons = {
    workspace_busy: '工作空间在该时间已有循环运行，已错过且不会补跑',
    daemon_offline: 'daemon 未运行，已错过且不会补跑',
    daemon_interrupted: 'daemon 退出时运行被中断',
    model_unavailable: '所选模型已不可用，循环已停用',
    workspace_unavailable: '工作空间不可用',
  };
  return {
    label: labels[run.status] || run.status || '未知',
    reason: reasons[run.reason] || run.reason || '',
    tone: run.status === 'completed' ? 'ok' : run.status === 'failed' ? 'error' : run.status === 'missed' ? 'warn' : 'active',
  };
}
