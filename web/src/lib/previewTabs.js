import { normalizeTreePath } from './fileTreeChangeStatus.js';

export const PREVIEW_TAB_TYPES = Object.freeze({
  FILE: 'file',
  SESSION_CHANGES: 'session-changes',
  GIT_CHANGES: 'git-changes',
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

function gitChangesTabKey(sessionId) {
  return `git-changes:${sessionId}`;
}

// session-changes 与 git-changes 是同一个「变更」概念的两种数据源(非 git
// 仓库走会话内 hunks 聚合,git 仓库走 numstat/diff),同一 session 二选一,
// 因此共用 `changeTabsBySession` 这一个槽位。close / activate 只需按 tab.key
// 的前缀区分,存储/查询逻辑不必翻倍。
function isChangeTabKey(tabKey) {
  return typeof tabKey === 'string'
    && (tabKey.startsWith('session-changes:') || tabKey.startsWith('git-changes:'));
}

function nextActiveAfterClose(tabs, closedKey) {
  const index = tabs.findIndex((tab) => tab.key === closedKey);
  if (index < 0) return tabs[0]?.key || '';
  return tabs[index + 1]?.key || tabs[index - 1]?.key || '';
}

export function openFileTab(state, { scopeKey = '', sessionId = '', cwd = '', path = '', line = null } = {}) {
  const normalizedPath = normalizeTreePath(path);
  if (!scopeKey || !normalizedPath) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const tabs = source.fileTabsByScope?.[scopeKey] || [];
  const key = fileTabKey(scopeKey, normalizedPath);
  // 聊天正文的 foo.cpp:42 链接带行号进来:tab 记录 line + lineRevision(仿
  // expandedFileRevision 模式)。revision 每次带行号打开都递增,让重复点击同一
  // 链接也能触发预览重新滚动。不带行号的打开(文件树点击)不清已有定位。
  const focusLine = Number.isFinite(line) && line > 0 ? Math.floor(line) : null;
  const existing = tabs.find((tab) => tab.key === key);
  let nextTabs;
  if (existing) {
    nextTabs = focusLine == null ? tabs : tabs.map((tab) => (tab.key === key
      ? { ...tab, line: focusLine, lineRevision: (tab.lineRevision || 0) + 1 }
      : tab));
  } else {
    nextTabs = [...tabs, {
      key,
      type: PREVIEW_TAB_TYPES.FILE,
      scopeKey,
      cwd,
      path: normalizedPath,
      title: normalizedPath.split('/').pop() || normalizedPath,
      ...(focusLine != null ? { line: focusLine, lineRevision: 1 } : {}),
    }];
  }
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

// git 级变更详情页签(仿 openSessionChangesTab):同一 session 单页签,
// 点不同文件只更新 expandedFile + 自增 revision(触发详情栏滚动/展开),
// cwd/base 决定详情栏拉哪一份 diff。
export function openGitChangesTab(state, {
  scopeKey = '',
  sessionId = '',
  cwd = '',
  base,
  expandedFile = '',
  fileCount,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const key = gitChangesTabKey(sessionId);
  const previousTab = source.changeTabsBySession?.[sessionId];
  const previousIsGit = previousTab?.type === PREVIEW_TAB_TYPES.GIT_CHANGES;
  // base 缺省(在详情栏内点文件仅换展开文件时)保留页签原 base,别把比较基线清空。
  const nextBase = base !== undefined ? base : (previousIsGit ? previousTab?.base : '') || '';
  const nextFileCount = fileCount !== undefined
    ? (Number.isFinite(Number(fileCount)) ? Number(fileCount) : 0)
    : (previousIsGit ? (previousTab?.fileCount || 0) : 0);
  const tab = {
    key,
    type: PREVIEW_TAB_TYPES.GIT_CHANGES,
    sessionId,
    cwd: cwd || (previousIsGit ? previousTab?.cwd : '') || '',
    base: nextBase,
    expandedFile: normalizeTreePath(expandedFile),
    expandedFileRevision: (Number(previousIsGit ? previousTab?.expandedFileRevision : 0) || 0) + 1,
    fileCount: nextFileCount,
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

// 更新已打开的 git 变更页签的 base / fileCount,不动 expandedFile / revision
// (基线切换或文件数刷新时用)。页签不存在或不是 git 类型则原样返回。
export function updateGitChangesTab(state, {
  sessionId = '',
  base,
  fileCount,
} = {}) {
  if (!sessionId) return state || {};
  const source = state && typeof state === 'object' ? state : {};
  const existing = source.changeTabsBySession?.[sessionId];
  if (!existing || existing.type !== PREVIEW_TAB_TYPES.GIT_CHANGES) return source;
  const nextTab = { ...existing };
  if (base !== undefined) nextTab.base = base;
  if (fileCount !== undefined) {
    nextTab.fileCount = Number.isFinite(Number(fileCount)) ? Number(fileCount) : existing.fileCount;
  }
  return {
    ...source,
    changeTabsBySession: {
      ...(source.changeTabsBySession || {}),
      [sessionId]: nextTab,
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
  // 只更新会话级 hunks 页签;git 页签共用同一槽位,别被会话变更计数误改
  // (git 文件数由 GitChangesPanel 自己经 updateGitChangesTab 推送)。
  if (!existing || existing.type !== PREVIEW_TAB_TYPES.SESSION_CHANGES) return source;
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
  if (isChangeTabKey(tabKey)) {
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
  if (isChangeTabKey(tabKey) && sessionId) {
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
