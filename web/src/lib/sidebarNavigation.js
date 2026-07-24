export const SIDEBAR_NAV_ITEMS = Object.freeze([
  Object.freeze({ id: 'new-task', label: '新建任务', icon: 'newSession', callback: 'onNewTask' }),
  Object.freeze({ id: 'new-loop', label: '循环任务', icon: 'alarm', callback: 'onNewLoop' }),
  Object.freeze({ id: 'search-tasks', label: '搜索任务', icon: 'search', callback: 'onSearchTasks' }),
]);

export const SIDEBAR_CUSTOM_ITEMS = Object.freeze([
  Object.freeze({ id: 'mcp', label: 'MCP 服务器', icon: 'mcp', settingsSection: 'mcp' }),
  Object.freeze({ id: 'skills', label: '技能', icon: 'lightbulb', settingsSection: 'skills' }),
  Object.freeze({ id: 'experts', label: '专家组件', icon: 'brain', action: 'experts' }),
]);

export const DEFAULT_SIDEBAR_CUSTOM_EXPANDED = false;

export const SIDEBAR_SECTION_IDS = Object.freeze({
  PINNED: 'pinned',
  TASKS: 'tasks',
  WORKSPACES: 'workspaces',
});

export const SIDEBAR_SECTION_LABELS = Object.freeze({
  [SIDEBAR_SECTION_IDS.PINNED]: '置顶任务',
  [SIDEBAR_SECTION_IDS.TASKS]: '任务',
  [SIDEBAR_SECTION_IDS.WORKSPACES]: '工作区',
});

export const DEFAULT_SIDEBAR_SECTION_EXPANSION = Object.freeze({
  [SIDEBAR_SECTION_IDS.PINNED]: true,
  [SIDEBAR_SECTION_IDS.TASKS]: true,
  [SIDEBAR_SECTION_IDS.WORKSPACES]: true,
});

export const SIDEBAR_DISCLOSURE_ICON = Object.freeze({
  width: 16,
  height: 16,
  viewBox: '0 0 16 16',
  path: 'M4 6L8 10L12 6',
  stroke: 'currentColor',
  strokeWidth: 1.2,
  strokeLinecap: 'round',
  strokeLinejoin: 'round',
});

export function validateSidebarSectionExpansion(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  return Object.values(SIDEBAR_SECTION_IDS).every((id) => typeof value[id] === 'boolean');
}

export function sidebarSectionCounts({
  pinnedSessions = [],
  noWorkspaceSessions = [],
  workspaces = [],
} = {}) {
  return {
    [SIDEBAR_SECTION_IDS.PINNED]: Array.isArray(pinnedSessions) ? pinnedSessions.length : 0,
    [SIDEBAR_SECTION_IDS.TASKS]: Array.isArray(noWorkspaceSessions) ? noWorkspaceSessions.length : 0,
    [SIDEBAR_SECTION_IDS.WORKSPACES]: Array.isArray(workspaces) ? workspaces.length : 0,
  };
}

export function sidebarSectionIsVisible(count) {
  return Number.isFinite(count) && count > 0;
}

export function sidebarSectionTitle(sectionId, count) {
  const label = SIDEBAR_SECTION_LABELS[sectionId] || '';
  const safeCount = Number.isFinite(count) && count >= 0 ? Math.floor(count) : 0;
  return `${label} (${safeCount})`;
}

export function sidebarCustomTotalCount(counts = {}) {
  const available = SIDEBAR_CUSTOM_ITEMS
    .map((item) => counts?.[item.id])
    .filter((count) => Number.isFinite(count) && count >= 0)
    .map((count) => Math.floor(count));
  return available.length > 0
    ? available.reduce((total, count) => total + count, 0)
    : null;
}
