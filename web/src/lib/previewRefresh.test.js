import assert from 'node:assert/strict';
import {
  activeFileWasChanged,
  nextAutoPreviewRefresh,
  previewPathMatches,
} from './previewRefresh.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('previewPathMatches matches Windows absolute, relative, separator, and case variants', () => {
  const preview = { cwd: 'C:\\Repo', path: 'src/main.cpp' };
  assert.equal(previewPathMatches(preview, 'C:/repo/src/main.cpp'), true);
  assert.equal(previewPathMatches(preview, 'src\\MAIN.cpp'), true);
  assert.equal(previewPathMatches(preview, '.\\src\\main.cpp'), true);
  assert.equal(previewPathMatches(preview, 'C:/repo/src/other.cpp'), false);
});

run('previewPathMatches keeps POSIX path comparison case-sensitive', () => {
  const preview = { cwd: '/repo', path: 'src/Main.cpp' };
  assert.equal(previewPathMatches(preview, '/repo/src/Main.cpp'), true);
  assert.equal(previewPathMatches(preview, '/repo/src/main.cpp'), false);
  assert.equal(previewPathMatches(preview, '/Repo/src/Main.cpp'), false);
});

run('activeFileWasChanged only accepts active file tabs and matching paths', () => {
  assert.equal(activeFileWasChanged(
    { type: 'file', cwd: 'C:/repo', path: 'src/a.js' },
    ['src/b.js', 'C:/repo/src/a.js'],
  ), true);
  assert.equal(activeFileWasChanged({ type: 'git-changes' }, ['src/a.js']), false);
  assert.equal(activeFileWasChanged(null, ['src/a.js']), false);
});

run('nextAutoPreviewRefresh emits once on a matching busy-to-idle turn boundary', () => {
  const tab = { key: 'file:w:src/a.js', type: 'file', cwd: 'C:/repo', path: 'src/a.js' };
  let transition = nextAutoPreviewRefresh({}, { sid: 's1', busy: true, turnKey: 'u1', activeTab: tab, changedPaths: [] });
  assert.equal(transition.tabKey, '');
  transition = nextAutoPreviewRefresh(transition.state, {
    sid: 's1', busy: false, turnKey: 'u1', activeTab: tab, changedPaths: ['C:/repo/src/a.js'],
  });
  assert.equal(transition.tabKey, tab.key);

  const repeated = nextAutoPreviewRefresh(transition.state, {
    sid: 's1', busy: false, turnKey: 'u1', activeTab: tab, changedPaths: ['src/a.js'],
  });
  assert.equal(repeated.tabKey, '');
});

run('nextAutoPreviewRefresh resets across sessions and ignores nonmatching completed turns', () => {
  const tab = { key: 'file:w:src/a.js', type: 'file', cwd: '/repo', path: 'src/a.js' };
  const switched = nextAutoPreviewRefresh(
    { sid: 'old', busy: true, completedTurnKey: '' },
    { sid: 'new', busy: false, turnKey: 'u2', activeTab: tab, changedPaths: ['/repo/src/a.js'] },
  );
  assert.equal(switched.tabKey, '');
  const running = nextAutoPreviewRefresh(switched.state, {
    sid: 'new', busy: true, turnKey: 'u3', activeTab: tab, changedPaths: [],
  });
  const finished = nextAutoPreviewRefresh(running.state, {
    sid: 'new', busy: false, turnKey: 'u3', activeTab: tab, changedPaths: ['/repo/src/b.js'],
  });
  assert.equal(finished.tabKey, '');
});
