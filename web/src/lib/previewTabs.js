import { normalizeTreePath } from './fileTreeChangeStatus.js';

export const PREVIEW_TAB_TYPES = Object.freeze({
  FILE: 'file',
  SESSION_CHANGES: 'session-changes',
});

export function previewScopeKey({ cwd = '', workspaceHash = '' } = {}) {
  return workspaceHash || cwd || '';
}

function normalizePreviewCwd(cwd = '') {
  return String(cwd || '')
    .replace(/\\/g, '/')
    .replace(/^\/\/\?\//, '')
    .replace(/\/+$/g, '');
}

export function previewFileLocation({ cwd = '', path = '' } = {}) {
  const normalizedCwd = normalizePreviewCwd(cwd);
  const normalizedPath = normalizeTreePath(path);
  if (!normalizedPath) return { cwd: normalizedCwd, path: '' };
  if (normalizedCwd) return { cwd: normalizedCwd, path: normalizedPath };

  if (/^[A-Za-z]:\//.test(normalizedPath)) {
    const slash = normalizedPath.lastIndexOf('/');
    if (slash > 2 && slash < normalizedPath.length - 1) {
      return {
        cwd: normalizedPath.slice(0, slash),
        path: normalizedPath.slice(slash + 1),
      };
    }
  }

  return { cwd: normalizedCwd, path: normalizedPath };
}

export function visiblePreviewTabs(state, { scopeKey = '', sessionId = '' } = {}) {
  const source = state && typeof state === 'object' ? state : {};
  const fileTabs = source.fileTabsByScope?.[scopeKey] || [];
  const changeTab = sessionId ? source.changeTabsBySession?.[sessionId] : null;
  const naturalTabs = changeTab ? [...fileTabs, changeTab] : fileTabs.slice();
  return applyVisibleTabOrder(naturalTabs, source.tabOrderByView?.[viewKey(scopeKey, sessionId)]);
}

function viewKey(scopeKey, sessionId) {
  return `${scopeKey || ''}::${sessionId || ''}`;
}

function applyVisibleTabOrder(tabs, order) {
  if (!Array.isArray(order) || order.length === 0 || tabs.length <= 1) return tabs.slice();
  const tabsByKey = new Map(tabs.map((tab) => [tab.key, tab]));
  const used = new Set();
  const ordered = [];
  order.forEach((key) => {
    if (used.has(key) || !tabsByKey.has(key)) return;
    used.add(key);
    ordered.push(tabsByKey.get(key));
  });
  tabs.forEach((tab) => {
    if (used.has(tab.key)) return;
    used.add(tab.key);
    ordered.push(tab);
  });
  return ordered;
}

function removeKeysFromOrders(orderMap, keysToRemove) {
  const keys = new Set(keysToRemove.filter(Boolean));
  if (!orderMap || keys.size === 0) return orderMap || {};
  const next = {};
  Object.entries(orderMap).forEach(([key, order]) => {
    if (!Array.isArray(order)) return;
    const filtered = order.filter((tabKey) => !keys.has(tabKey));
    if (filtered.length > 0) next[key] = filtered;
  });
  return next;
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
  const previousTab = source.changeTabsBySession?.[sessionId];
  const tab = {
    key,
    type: PREVIEW_TAB_TYPES.SESSION_CHANGES,
    sessionId,
    expandedFile: normalizeTreePath(expandedFile),
    expandedFileRevision: (Number(previousTab?.expandedFileRevision) || 0) + 1,
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

export function reorderPreviewTab(state, {
  scopeKey = '',
  sessionId = '',
  sourceKey = '',
  targetKey = '',
  placement = 'before',
} = {}) {
  const source = state && typeof state === 'object' ? state : {};
  if (!sourceKey || !targetKey || sourceKey === targetKey) return source;
  const tabs = visiblePreviewTabs(source, { scopeKey, sessionId });
  const keys = tabs.map((tab) => tab.key);
  if (!keys.includes(sourceKey) || !keys.includes(targetKey)) return source;

  const withoutSource = keys.filter((key) => key !== sourceKey);
  const targetIndex = withoutSource.indexOf(targetKey);
  if (targetIndex < 0) return source;
  const insertIndex = placement === 'after' ? targetIndex + 1 : targetIndex;
  const nextOrder = [
    ...withoutSource.slice(0, insertIndex),
    sourceKey,
    ...withoutSource.slice(insertIndex),
  ];

  return {
    ...source,
    tabOrderByView: {
      ...(source.tabOrderByView || {}),
      [viewKey(scopeKey, sessionId)]: nextOrder,
    },
  };
}

export function closePreviewTab(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const visibleBeforeClose = visiblePreviewTabs(source, { scopeKey, sessionId });
  if (tabKey.startsWith('file:')) {
    const tabs = source.fileTabsByScope?.[scopeKey] || [];
    const nextTabs = tabs.filter((tab) => tab.key !== tabKey);
    const nextFileTabs = { ...(source.fileTabsByScope || {}) };
    if (nextTabs.length > 0) nextFileTabs[scopeKey] = nextTabs;
    else delete nextFileTabs[scopeKey];
    const nextVisibleActive = nextActiveAfterClose(visibleBeforeClose, tabKey);
    return {
      ...source,
      fileTabsByScope: nextFileTabs,
      activeTabByScope: {
        ...(source.activeTabByScope || {}),
        [scopeKey]: nextVisibleActive.startsWith('file:')
          ? nextVisibleActive
          : nextActiveAfterClose(tabs, tabKey),
      },
      activeTabByView: {
        ...(source.activeTabByView || {}),
        [viewKey(scopeKey, sessionId)]: nextVisibleActive,
      },
      tabOrderByView: removeKeysFromOrders(source.tabOrderByView, [tabKey]),
    };
  }
  if (tabKey.startsWith('session-changes:')) {
    const nextChangeTabs = { ...(source.changeTabsBySession || {}) };
    delete nextChangeTabs[sessionId];
    const nextActive = { ...(source.activeTabBySession || {}) };
    delete nextActive[sessionId];
    const nextActiveByView = { ...(source.activeTabByView || {}) };
    nextActiveByView[viewKey(scopeKey, sessionId)] = nextActiveAfterClose(
      visibleBeforeClose,
      tabKey,
    );
    return {
      ...source,
      changeTabsBySession: nextChangeTabs,
      activeTabBySession: nextActive,
      activeTabByView: nextActiveByView,
      tabOrderByView: removeKeysFromOrders(source.tabOrderByView, [tabKey]),
    };
  }
  return source;
}

export function closeVisiblePreviewTabs(state, { scopeKey = '', sessionId = '' } = {}) {
  const source = state && typeof state === 'object' ? state : {};
  const closedKeys = visiblePreviewTabs(source, { scopeKey, sessionId }).map((tab) => tab.key);
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
    tabOrderByView: removeKeysFromOrders(source.tabOrderByView, closedKeys),
  };
}

export function closeVisiblePreviewTabsConfirmationMessage(tabCount = 0) {
  const count = Number(tabCount);
  if (!Number.isFinite(count) || count <= 0) return '';
  return `关闭预览面板会关闭当前预览区域的全部 ${count} 个标签页。是否继续？`;
}

export function closeOtherPreviewTabs(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const visible = visiblePreviewTabs(source, { scopeKey, sessionId });
  const closedKeys = visible.filter((tab) => tab.key !== tabKey).map((tab) => tab.key);
  if (closedKeys.length === 0) return source;
  let next = source;
  for (const key of closedKeys) {
    next = closePreviewTab(next, { scopeKey, sessionId, tabKey: key });
  }
  return {
    ...next,
    activeTabByView: {
      ...(next.activeTabByView || {}),
      [viewKey(scopeKey, sessionId)]: tabKey,
    },
  };
}

export function closePreviewTabsToRight(state, { scopeKey = '', sessionId = '', tabKey = '' } = {}) {
  if (!tabKey) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const visible = visiblePreviewTabs(source, { scopeKey, sessionId });
  const index = visible.findIndex((tab) => tab.key === tabKey);
  if (index < 0 || index >= visible.length - 1) return source;
  const closedKeys = visible.slice(index + 1).map((tab) => tab.key);
  let next = source;
  for (const key of closedKeys) {
    next = closePreviewTab(next, { scopeKey, sessionId, tabKey: key });
  }
  return next;
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
