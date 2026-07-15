const ACTIVE_STATES = new Set(['pending', 'running']);

const PHASE_LABELS = {
  checking: '正在检查更新',
  downloading: '正在下载安装包',
  verifying: '正在校验安装包',
  extracting: '正在解压安装包',
  installing: '正在安装更新',
  complete: '升级安装完成',
};

const PHASE_PROGRESS = {
  checking: 5,
  verifying: 78,
  extracting: 86,
  installing: 95,
  complete: 100,
};

export function updateJobIsActive(job) {
  return ACTIVE_STATES.has(job?.state);
}

export function updateJobPhaseLabel(job) {
  if (job?.state === 'failed') return '升级失败';
  if (job?.state === 'succeeded') return '升级安装完成';
  return PHASE_LABELS[job?.phase] || '正在准备升级';
}

export function updateJobProgress(job) {
  if (!job) return 0;
  if (job.state === 'succeeded') return 100;
  if (job.phase === 'downloading') {
    const raw = Number(job.percent);
    const bounded = Number.isFinite(raw) ? Math.max(0, Math.min(100, raw)) : 0;
    return Math.round(10 + bounded * 0.6);
  }
  return PHASE_PROGRESS[job.phase] ?? 0;
}

export function updateRestartMessage(job) {
  if (job?.state !== 'succeeded' || !job?.restart_required) return '';
  return '升级已安装。请完全退出并重新启动 ACECode；当前窗口仍在运行旧版本。';
}

export function desktopUpdateRestartAvailable(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  return typeof win?.aceDesktop_restartApp === 'function';
}

function parseDesktopRestartResult(raw) {
  if (typeof raw !== 'string') return raw;
  const text = raw.trim();
  return text ? JSON.parse(text) : null;
}

export async function requestDesktopUpdateRestart(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!desktopUpdateRestartAvailable(win)) {
    throw new Error('当前运行模式不支持自动重启，请完全退出后重新启动 ACECode');
  }
  const result = parseDesktopRestartResult(await win.aceDesktop_restartApp());
  if (!result || result.ok !== true) {
    throw new Error(String(result?.error || 'ACECode 无法自动重启'));
  }
  return result;
}

export function updateDialogMode(job) {
  if (!job) return 'confirm';
  if (updateJobIsActive(job)) return 'running';
  if (job.state === 'succeeded') return 'success';
  if (job.state === 'failed') return 'failure';
  return 'confirm';
}
