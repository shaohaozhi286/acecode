import assert from 'node:assert/strict';
import {
  DEFAULT_SIDEBAR_SECTION_EXPANSION,
  DEFAULT_SIDEBAR_CUSTOM_EXPANDED,
  SIDEBAR_CUSTOM_ITEMS,
  SIDEBAR_DISCLOSURE_ICON,
  SIDEBAR_NAV_ITEMS,
  SIDEBAR_SECTION_IDS,
  SIDEBAR_SECTION_LABELS,
  sidebarSectionCounts,
  sidebarSectionIsVisible,
  sidebarSectionTitle,
  sidebarCustomMaxCount,
  validateSidebarSectionExpansion,
} from './sidebarNavigation.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('sidebar fixed navigation keeps the confirmed order and callbacks', () => {
  assert.deepEqual(SIDEBAR_NAV_ITEMS, [
    { id: 'new-task', label: '新建任务', icon: 'newSession', callback: 'onNewTask' },
    { id: 'new-loop', label: '循环任务', icon: 'alarm', callback: 'onNewLoop' },
    { id: 'search-tasks', label: '搜索任务', icon: 'search', callback: 'onSearchTasks' },
  ]);
});

test('sidebar custom settings keep the restored order and default collapsed state', () => {
  assert.deepEqual(SIDEBAR_CUSTOM_ITEMS, [
    { id: 'mcp', label: 'MCP 服务器', icon: 'mcp', settingsSection: 'mcp' },
    { id: 'models', label: '模型', icon: 'code', settingsSection: 'models' },
    { id: 'skills', label: '技能', icon: 'lightbulb', settingsSection: 'skills' },
  ]);
  assert.equal(DEFAULT_SIDEBAR_CUSTOM_EXPANDED, false);
});

test('sidebar custom heading uses the greatest available count', () => {
  assert.equal(sidebarCustomMaxCount({ mcp: 7, models: 38, skills: 12 }), 38);
  assert.equal(sidebarCustomMaxCount({ mcp: null, models: 3.9, skills: Number.NaN }), 3);
  assert.equal(sidebarCustomMaxCount({ mcp: -1, models: undefined, skills: null }), null);
});

test('sidebar sections use confirmed labels and default expanded state', () => {
  assert.deepEqual(SIDEBAR_SECTION_LABELS, {
    pinned: '置顶任务',
    tasks: '任务',
    workspaces: '工作区',
  });
  assert.deepEqual(DEFAULT_SIDEBAR_SECTION_EXPANSION, {
    pinned: true,
    tasks: true,
    workspaces: true,
  });
  assert.equal(validateSidebarSectionExpansion(DEFAULT_SIDEBAR_SECTION_EXPANSION), true);
  assert.equal(validateSidebarSectionExpansion({ pinned: true, tasks: true }), false);
  assert.equal(validateSidebarSectionExpansion({ pinned: true, tasks: true, workspaces: 'yes' }), false);
});

test('sidebar section counts separate pinned tasks, no-workspace tasks, and workspaces', () => {
  const counts = sidebarSectionCounts({
    pinnedSessions: [{ id: 'p1' }, { id: 'p2' }],
    noWorkspaceSessions: [{ id: 'n1' }],
    workspaces: [{ hash: 'w1' }, { hash: 'w2' }, { hash: 'w3' }],
  });
  assert.deepEqual(counts, { pinned: 2, tasks: 1, workspaces: 3 });
  assert.equal(sidebarSectionTitle(SIDEBAR_SECTION_IDS.PINNED, counts.pinned), '置顶任务 (2)');
  assert.equal(sidebarSectionTitle(SIDEBAR_SECTION_IDS.TASKS, counts.tasks), '任务 (1)');
  assert.equal(sidebarSectionTitle(SIDEBAR_SECTION_IDS.WORKSPACES, counts.workspaces), '工作区 (3)');
  assert.deepEqual(sidebarSectionCounts(), { pinned: 0, tasks: 0, workspaces: 0 });
});

test('sidebar sections render only when their count is positive', () => {
  assert.equal(sidebarSectionIsVisible(0), false);
  assert.equal(sidebarSectionIsVisible(-1), false);
  assert.equal(sidebarSectionIsVisible(Number.NaN), false);
  assert.equal(sidebarSectionIsVisible(undefined), false);
  assert.equal(sidebarSectionIsVisible(1), true);
  assert.equal(sidebarSectionIsVisible(400), true);
});

test('sidebar disclosure icon preserves the exact confirmed SVG geometry', () => {
  assert.deepEqual(SIDEBAR_DISCLOSURE_ICON, {
    width: 16,
    height: 16,
    viewBox: '0 0 16 16',
    path: 'M4 6L8 10L12 6',
    stroke: 'currentColor',
    strokeWidth: 1.2,
    strokeLinecap: 'round',
    strokeLinejoin: 'round',
  });
});
