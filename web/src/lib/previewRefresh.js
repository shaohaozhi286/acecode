function rawPath(value) {
  return String(value || '').trim().replace(/\\/g, '/').replace(/^\/\/\?\//, '');
}

function normalizedPath(value) {
  const raw = rawPath(value);
  if (!raw) return '';
  const prefix = raw.startsWith('//') ? '//' : raw.startsWith('/') ? '/' : '';
  const body = raw
    .replace(/^\/+/, '')
    .replace(/\/+/g, '/')
    .split('/')
    .filter((part) => part && part !== '.')
    .join('/');
  return `${prefix}${body}`;
}

function isWindowsPath(value) {
  const path = rawPath(value);
  return /^[A-Za-z]:\//.test(path) || path.startsWith('//');
}

function isAbsolutePath(value) {
  const path = rawPath(value);
  return /^[A-Za-z]:\//.test(path) || path.startsWith('/');
}

function resolvedPreviewPath(cwd, path) {
  const normalized = normalizedPath(path);
  if (!normalized || isAbsolutePath(path)) return normalized;
  const root = normalizedPath(cwd);
  return root ? `${root.replace(/\/$/, '')}/${normalized.replace(/^\/+/, '')}` : normalized;
}

export function previewPathMatches({ cwd = '', path = '' } = {}, changedPath = '') {
  if (!path || !changedPath) return false;
  const active = resolvedPreviewPath(cwd, path);
  const changed = resolvedPreviewPath(cwd, changedPath);
  if (!active || !changed) return false;
  const caseInsensitive = isWindowsPath(cwd) || isWindowsPath(path) || isWindowsPath(changedPath);
  return caseInsensitive
    ? active.toLowerCase() === changed.toLowerCase()
    : active === changed;
}

export function activeFileWasChanged(activeTab, changedPaths) {
  if (activeTab?.type !== 'file' || !Array.isArray(changedPaths)) return false;
  return changedPaths.some((changedPath) => previewPathMatches(activeTab, changedPath));
}

export function nextAutoPreviewRefresh(previous = {}, {
  sid = '',
  busy = false,
  turnKey = '',
  activeTab = null,
  changedPaths = [],
} = {}) {
  if (previous.sid !== sid) {
    return {
      state: { sid, busy, completedTurnKey: '' },
      tabKey: '',
    };
  }

  const state = {
    sid,
    busy,
    completedTurnKey: previous.completedTurnKey || '',
  };
  const turnJustFinished = !!previous.busy && !busy;
  if (!turnJustFinished || !turnKey || state.completedTurnKey === turnKey) {
    return { state, tabKey: '' };
  }
  state.completedTurnKey = turnKey;
  return {
    state,
    tabKey: activeFileWasChanged(activeTab, changedPaths) ? activeTab.key || '' : '',
  };
}
