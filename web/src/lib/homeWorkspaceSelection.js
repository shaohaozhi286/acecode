export const HOME_WORKSPACE_SELECTION_STORAGE_KEY = 'acecode.homeWorkspaceSelection.v1';
export const DEFAULT_HOME_WORKSPACE_SELECTION = { workspaceHash: '' };

export function validateHomeWorkspaceSelection(value) {
  return !!value
    && typeof value === 'object'
    && !Array.isArray(value)
    && typeof value.workspaceHash === 'string';
}

export function noHomeWorkspaceOption() {
  return {
    hash: '',
    cwd: '',
    name: '不使用工作区',
    noWorkspace: true,
    active: false,
    contextId: 'default',
  };
}

function optionHashes(options = []) {
  return new Set((Array.isArray(options) ? options : [])
    .map((w) => String(w?.hash || ''))
    .filter(Boolean));
}

export function resolveHomeWorkspaceHash({
  preferredHash = '',
  explicitHash = '',
  previousHash = '',
  options = [],
} = {}) {
  const hashes = optionHashes(options);
  if (explicitHash && hashes.has(explicitHash)) return explicitHash;
  if (preferredHash && hashes.has(preferredHash)) return preferredHash;
  if (previousHash && hashes.has(previousHash)) return previousHash;
  return '';
}

export function homeWorkspaceOptionForHash(options = [], hash = '') {
  if (!hash) return noHomeWorkspaceOption();
  return (Array.isArray(options) ? options : []).find((w) => w?.hash === hash)
    || noHomeWorkspaceOption();
}

export function parseDesktopBridgeResult(value) {
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

export async function readDesktopHomeWorkspaceHash(win = globalThis.window) {
  const fn = win?.aceDesktop_getHomeWorkspaceHash;
  if (typeof fn !== 'function') return null;
  try {
    const result = parseDesktopBridgeResult(await fn());
    if (result?.error) return null;
    return typeof result?.workspace_hash === 'string' ? result.workspace_hash : '';
  } catch {
    return null;
  }
}

export async function writeDesktopHomeWorkspaceHash(hash = '', win = globalThis.window) {
  const fn = win?.aceDesktop_setHomeWorkspaceHash;
  if (typeof fn !== 'function') return false;
  try {
    const result = parseDesktopBridgeResult(await fn(String(hash || '')));
    return !!result?.ok && !result?.error;
  } catch {
    return false;
  }
}
