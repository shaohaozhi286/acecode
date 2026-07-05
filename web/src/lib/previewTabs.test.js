import assert from 'node:assert/strict';
import {
  activePreviewTab,
  activatePreviewTab,
  closePreviewTab,
  closeVisiblePreviewTabs,
  closeVisiblePreviewTabsConfirmationMessage,
  openFileTab,
  openSessionChangesTab,
  previewFileLocation,
  reorderPreviewTab,
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

run('previewFileLocation splits absolute Windows paths when cwd is unavailable', () => {
  assert.deepEqual(
    previewFileLocation({ cwd: '', path: 'C:\\Users\\shao\\ttt\\hello.txt' }),
    { cwd: 'C:/Users/shao/ttt', path: 'hello.txt' },
  );
  assert.deepEqual(
    previewFileLocation({ cwd: 'C:/repo', path: 'src/main.cpp' }),
    { cwd: 'C:/repo', path: 'src/main.cpp' },
  );
  assert.deepEqual(
    previewFileLocation({ cwd: '/home/shao/repo/', path: 'src/main.cpp' }),
    { cwd: '/home/shao/repo', path: 'src/main.cpp' },
  );
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

run('openSessionChangesTab increments expanded file revision for repeated clicks', () => {
  let state = {};
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });
  const firstRevision = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).expandedFileRevision;

  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'src/a.cpp',
    fileCount: 2,
  });

  const active = activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  assert.equal(active.expandedFile, 'src/a.cpp');
  assert.equal(active.expandedFileRevision, firstRevision + 1);
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

run('reorderPreviewTab reorders visible file tabs', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'c.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });

  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[2].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });

  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.path),
    ['c.txt', 'a.txt', 'b.txt'],
  );
});

run('reorderPreviewTab lets session-change tab participate in visible order', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openSessionChangesTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    expandedFile: 'a.txt',
    fileCount: 1,
  });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  const fileTab = tabs.find((tab) => tab.type === 'file');
  const changeTab = tabs.find((tab) => tab.type === 'session-changes');

  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: changeTab.key,
    targetKey: fileTab.key,
    placement: 'before',
  });

  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.type),
    ['session-changes', 'file'],
  );
  assert.equal(state.changeTabsBySession.s1.sessionId, 's1');
});

run('activatePreviewTab preserves reordered visible order', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'c.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[2].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });
  const bKey = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' })
    .find((tab) => tab.path === 'b.txt').key;
  state = activatePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: bKey });

  assert.equal(activePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1' }).path, 'b.txt');
  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.path),
    ['c.txt', 'a.txt', 'b.txt'],
  );
});

run('closePreviewTab removes closed keys from stored visible order', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'c.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[2].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });

  state = closePreviewTab(state, { scopeKey: 'workspace-a', sessionId: 's1', tabKey: tabs[2].key });

  assert.deepEqual(
    visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).map((tab) => tab.path),
    ['a.txt', 'b.txt'],
  );
  assert.equal(
    Object.values(state.tabOrderByView || {}).some((order) => order.includes(tabs[2].key)),
    false,
  );
});

run('closeVisiblePreviewTabs clears visible order keys', () => {
  let state = {};
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'a.txt' });
  state = openFileTab(state, { scopeKey: 'workspace-a', sessionId: 's1', cwd: 'C:/a', path: 'b.txt' });
  const tabs = visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });
  state = reorderPreviewTab(state, {
    scopeKey: 'workspace-a',
    sessionId: 's1',
    sourceKey: tabs[1].key,
    targetKey: tabs[0].key,
    placement: 'before',
  });

  state = closeVisiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' });

  assert.equal(visiblePreviewTabs(state, { scopeKey: 'workspace-a', sessionId: 's1' }).length, 0);
  assert.deepEqual(state.tabOrderByView, {});
});
