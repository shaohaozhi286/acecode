import { normalizeTreePath } from './fileTreeChangeStatus.js';

export const PREVIEW_TAB_TYPES = Object.freeze({
  FILE: 'file',
  SESSION_CHANGES: 'session-changes',
});

export function previewScopeKey({ cwd = '', workspaceHash = '' } = {}) {
  return workspaceHash || cwd || '';
}

export function visiblePreviewTabs(state, { scopeKey = '', sessionId = '' } = {}) {
  const source = state && typeof state === 'object' ? state : {};
  const fileTabs = source.fileTabsByScope?.[scopeKey] || [];
  const changeTab = sessionId ? source.changeTabsBySession?.[sessionId] : null;
  return changeTab ? [...fileTabs, changeTab] : fileTabs.slice();
}

function viewKey(scopeKey, sessionId) {
  return `${scopeKey || ''}::${sessionId || ''}`;
}

function activeKeyFor(state, scopeKey, sessionId) {
  const source = state && typeof state === 'object' ? state : {};
  const activeByView = source.activeTabByView?.[viewKey(scopeKey, sessionId)];
  if (activeByView) return activeByView;
  if (sessionId && source.activeTabBySession?.[sessionId]) return source.activeTabBySession[sessionId];
  return source.activeTabByScope?.[scopeKey] || '';
}

export function activePreviewTab(state, context = {}) {
  const tabs = visiblePreviewTabs(state, context);
  const activeKey = activeKeyFor(state, context.scopeKey || '', context.sessionId || '');
  return tabs.find((tab) => tab.key === activeKey) || tabs[0] || null;
}

function fileTabKey(scopeKey, path) {
  return `file:${scopeKey}:${normalizeTreePath(path)}`;
}

function sessionChangesTabKey(sessionId) {
  return `session-changes:${sessionId}`;
}

function nextActiveAfterClose(tabs, closedKey) {
  const index = tabs.findIndex((tab) => tab.key === closedKey);
  if (index < 0) return tabs[0]?.key || '';
  return tabs[index + 1]?.key || tabs[index - 1]?.key || '';
}

export function openFileTab(state, { scopeKey = '', sessionId = '', cwd = '', path = '' } = {}) {
  const normalizedPath = normalizeTreePath(path);
  if (!scopeKey || !normalizedPath) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const tabs = source.fileTabsByScope?.[scopeKey] || [];
  const key = fileTabKey(scopeKey, normalizedPath);
  const exists = tabs.some((tab) => tab.key === key);
  const nextTabs = exists ? tabs : [...tabs, {
    key,
    type: PREVIEW_TAB_TYPES.FILE,
    scopeKey,
    cwd,
    path: normalizedPath,
    title: normalizedPath.split('/').pop() || normalizedPath,
  }];
  return {
    ...source,
    fileTabsByScope: {
      ...(source.fileTabsByScope || {}),
      [scopeKey]: nextTabs,
    },
    activeTabByScope: {
      ...(source.activeTabByScope || {}),
      [scopeKey]: key,
    },
    activeTabByView: {
      ...(source.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: key,
    },
  };
}

export function openSessionChangesTab(state, {
  scopeKey = '',
  sessionId = '',
  expandedFile = '',
  fileCount = 0,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const key = sessionChangesTabKey(sessionId);
  const tab = {
    key,
    type: PREVIEW_TAB_TYPES.SESSION_CHANGES,
    sessionId,
    expandedFile: normalizeTreePath(expandedFile),
    fileCount: Number.isFinite(Number(fileCount)) ? Number(fileCount) : 0,
  };
  return {
    ...source,
    changeTabsBySession: {
      ...(source.changeTabsBySession || {}),
      [sessionId]: tab,
    },
    activeTabBySession: {
      ...(source.activeTabBySession || {}),
      [sessionId]: key,
    },
    activeTabByView: {
      ...(source.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: key,
    },
  };
}

export function updateSessionChangesTab(state, {
  sessionId = '',
  fileCount = 0,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const existing = source.changeTabsBySession?.[sessionId];
  if (!existing) return source;
  return {
    ...source,
    changeTabsBySession: {
      ...(source.changeTabsBySession || {}),
      [sessionId]: {
        ...existing,
        fileCount: Number.isFinite(Number(fileCount)) ? Number(fileCount) : 0,
      },
    },
  };
}

export function closePreviewTab(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  if (tabKey.startsWith('file:')) {
    const tabs = source.fileTabsByScope?.[scopeKey] || [];
    const nextTabs = tabs.filter((tab) => tab.key !== tabKey);
    const nextFileTabs = { ...(source.fileTabsByScope || {}) };
    if (nextTabs.length > 0) nextFileTabs[scopeKey] = nextTabs;
    else delete nextFileTabs[scopeKey];
    return {
      ...source,
      fileTabsByScope: nextFileTabs,
      activeTabByScope: {
        ...(source.activeTabByScope || {}),
        [scopeKey]: nextActiveAfterClose(tabs, tabKey),
      },
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: nextActiveAfterClose(visiblePreviewTabs(source, { scopeKey, sessionId }), tabKey),
      },
    };
  }
  if (tabKey.startsWith('session-changes:')) {
    const nextChangeTabs = { ...(source.changeTabsBySession || {}) };
    delete nextChangeTabs[sessionId];
    const nextActive = { ...(source.activeTabBySession || {}) };
    delete nextActive[sessionId];
    const nextActiveByView = { ...(source.activeTabByView || {}) };
    nextActiveByView[viewKey(scopeKey, sessionId)] = nextActiveAfterClose(
      visiblePreviewTabs(source, { scopeKey, sessionId }),
      tabKey,
    );
    return {
      ...source,
      changeTabsBySession: nextChangeTabs,
      activeTabBySession: nextActive,
      activeTabByView: nextActiveByView,
    };
  }
  return source;
}

export function closeVisiblePreviewTabs(state, { scopeKey = '', sessionId = '' } = {}) {
  const source = state && typeof state === 'object' ? state : {};
  const nextFileTabs = { ...(source.fileTabsByScope || {}) };
  const nextActiveByScope = { ...(source.activeTabByScope || {}) };
  const nextActiveByView = { ...(source.activeTabByView || {}) };
  delete nextFileTabs[scopeKey];
  delete nextActiveByScope[scopeKey];
  delete nextActiveByView[viewKey(scopeKey, sessionId)];

  const nextChangeTabs = { ...(source.changeTabsBySession || {}) };
  const nextActiveBySession = { ...(source.activeTabBySession || {}) };
  if (sessionId) {
    delete nextChangeTabs[sessionId];
    delete nextActiveBySession[sessionId];
  }

  return {
    ...source,
    fileTabsByScope: nextFileTabs,
    activeTabByScope: nextActiveByScope,
    changeTabsBySession: nextChangeTabs,
    activeTabBySession: nextActiveBySession,
    activeTabByView: nextActiveByView,
  };
}

export function activatePreviewTab(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  if (tabKey.startsWith('session-changes:') && sessionId) {
    return {
      ...source,
      activeTabBySession: {
        ...(source.activeTabBySession || {}),
        [sessionId]: tabKey,
      },
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: tabKey,
      },
    };
  }
  if (scopeKey) {
    return {
      ...source,
      activeTabByScope: {
        ...(source.activeTabByScope || {}),
        [scopeKey]: tabKey,
      },
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: tabKey,
      },
    };
  }
  return source;
}
