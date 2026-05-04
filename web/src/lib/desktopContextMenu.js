export const DESKTOP_CONTEXT_ACTIONS = Object.freeze({
  OPEN_IN_EXPLORER: 'open_in_explorer',
  PIN_SESSION: 'pin_session',
  UNPIN_SESSION: 'unpin_session',
  SELECT_ALL: 'select_all',
  COPY: 'copy',
  PASTE: 'paste',
  CUT: 'cut',
  INSPECT: 'inspect',
});

export const OPEN_IN_EXPLORER_TARGET_SELECTOR = '[data-desktop-open-in-explorer-path]';
export const SESSION_PIN_TARGET_SELECTOR = '[data-desktop-session-id]';
export const SESSION_PIN_TOGGLE_EVENT = 'acecode:session-pin-toggle';

export function buildDesktopContextMenuItems({ editable = false, hasSelection = false, debug = false, openInExplorer = false, sessionPinTarget = null } = {}) {
  const items = [];
  if (sessionPinTarget) {
    items.push(sessionPinTarget.pinned
      ? DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION
      : DESKTOP_CONTEXT_ACTIONS.PIN_SESSION);
  }
  if (openInExplorer) items.push(DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER);
  items.push(DESKTOP_CONTEXT_ACTIONS.SELECT_ALL);
  if (hasSelection) items.push(DESKTOP_CONTEXT_ACTIONS.COPY);
  if (editable) {
    items.push(DESKTOP_CONTEXT_ACTIONS.PASTE);
    items.push(DESKTOP_CONTEXT_ACTIONS.CUT);
  }
  if (debug) items.push(DESKTOP_CONTEXT_ACTIONS.INSPECT);
  return items;
}

export function openInExplorerTargetFromElement(target) {
  if (!target || typeof target.closest !== 'function') return null;
  const el = target.closest(OPEN_IN_EXPLORER_TARGET_SELECTOR);
  if (!el) return null;
  const path = typeof el.getAttribute === 'function'
    ? el.getAttribute('data-desktop-open-in-explorer-path')
    : el.dataset?.desktopOpenInExplorerPath;
  if (!path) return null;
  const kind = typeof el.getAttribute === 'function'
    ? el.getAttribute('data-desktop-open-in-explorer-kind') || ''
    : el.dataset?.desktopOpenInExplorerKind || '';
  return { path, kind };
}

export function sessionPinTargetFromElement(target) {
  if (!target || typeof target.closest !== 'function') return null;
  const el = target.closest(SESSION_PIN_TARGET_SELECTOR);
  if (!el) return null;
  const get = (name, datasetName) => (typeof el.getAttribute === 'function'
    ? el.getAttribute(name)
    : el.dataset?.[datasetName]);
  const sessionId = get('data-desktop-session-id', 'desktopSessionId') || '';
  if (!sessionId) return null;
  const workspaceHash = get('data-desktop-session-workspace', 'desktopSessionWorkspace') || '';
  const pinned = get('data-desktop-session-pinned', 'desktopSessionPinned') === 'true';
  return { sessionId, workspaceHash, pinned };
}

export function joinWorkspacePath(cwd = '', relativePath = '') {
  const base = String(cwd || '').replace(/\\/g, '/').replace(/[\\/]+$/g, '');
  const rel = String(relativePath || '').replace(/\\/g, '/').replace(/^[\\/]+/g, '');
  if (!base) return rel;
  return rel ? `${base}/${rel}` : base;
}

export function clampContextMenuPosition({
  x,
  y,
  width,
  height,
  viewportWidth,
  viewportHeight,
  margin = 6,
}) {
  const safeWidth = Math.max(0, Number(width) || 0);
  const safeHeight = Math.max(0, Number(height) || 0);
  const safeViewportWidth = Math.max(0, Number(viewportWidth) || 0);
  const safeViewportHeight = Math.max(0, Number(viewportHeight) || 0);
  const maxLeft = Math.max(margin, safeViewportWidth - safeWidth - margin);
  const maxTop = Math.max(margin, safeViewportHeight - safeHeight - margin);
  return {
    left: Math.min(Math.max(Number(x) || margin, margin), maxLeft),
    top: Math.min(Math.max(Number(y) || margin, margin), maxTop),
  };
}
