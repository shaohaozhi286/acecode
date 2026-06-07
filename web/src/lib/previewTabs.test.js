import assert from 'node:assert/strict';
import {
  activePreviewTab,
  closePreviewTab,
  closeVisiblePreviewTabs,
  closeVisiblePreviewTabsConfirmationMessage,
  openFileTab,
  openSessionChangesTab,
  updateSessionChangesTab,
  visiblePreviewTabs,
} from './previewTabs.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('openFileTab scopes files by workspace and reuses duplicate paths', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'src\\main.cpp' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'src/main.cpp' });
  state = openFileTab(state, { scopeKey: 'workspace-b', sessionId: 's2', cwd: 'C:/b', path: 'src/main.cpp' });

  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 1);
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-b', sessionId: 's2' }).length, 1);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).path, 'src/main.cpp');
});

run('openSessionChangesTab scopes change tab by session id', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's2',
    expandedFile: 'src/b.cpp',
    fileCount: 3,
  });

  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 1);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).expandedFile, 'src/a.cpp');
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's2' }).expandedFile, 'src/b.cpp');
});

run('openSessionChangesTab reuses the same session tab and updates clicked file', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/b.cpp',
    fileCount: 3,
  });

  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tabs.length, 1);
  assert.equal(tabs[0].expandedFile, 'src/b.cpp');
  assert.equal(tabs[0].fileCount, 3);
});

run('file tab can become active when session change tab also exists', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 1,
  });
  state = openFileTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    cwd: 'C:/a',
    path: 'README.md',
  });

  const active = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(active.type, 'file');
  assert.equal(active.path, 'README.md');
});

run('closePreviewTab activates nearest remaining tab and hides when empty', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  const first = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' })[0];
  const second = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' })[1];

  state = closePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: second.key });
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).key, first.key);

  state = closePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: first.key });
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 0);
  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }), null);
});

run('closeVisiblePreviewTabs clears current visible scope and session', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openSessionChangesTab(state, { scopeKey: 'workspace-a', sessionId: 's1', expandedFile: 'a.txt', fileCount: 1 });
  state = openFileTab(state, { scopeKey: 'workspace-b', sessionId: 's2', cwd: 'C:/b', path: 'b.txt' });

  state = closeVisiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 0);
  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-b', sessionId: 's2' }).length, 1);
});

run('closeVisiblePreviewTabsConfirmationMessage warns about all visible tabs', () => {
  assert.equal(
    closeVisiblePreviewTabsConfirmationMessage(3),
    '关闭预览面板会关闭当前预览区域的全部 3 个标签页。是否继续？',
  );
  assert.equal(closeVisiblePreviewTabsConfirmationMessage(0), '');
  assert.equal(closeVisiblePreviewTabsConfirmationMessage(-1), '');
});

run('updateSessionChangesTab updates count without replacing expanded file', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 1,
  });
  state = updateSessionChangesTab(state, { sessionId: 's1', fileCount: 4 });
  const tab = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(tab.fileCount, 4);
  assert.equal(tab.expandedFile, 'src/a.cpp');
});
