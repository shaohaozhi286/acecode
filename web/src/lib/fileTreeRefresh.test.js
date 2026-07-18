import assert from 'node:assert/strict';
import {
  beginFileTreeDirectoryRequest,
  fileTreeDirectoryEntriesEqual,
  fileTreeDirectoryRequestKey,
  fileTreeRefreshKeyFromItems,
  fileTreeReloadPaths,
  finishFileTreeDirectoryRequest,
  reconcileFileTreeDirectory,
} from './fileTreeRefresh.js';

function run(name, fn) {
  try {
    fn();
    console.log('ok - ' + name);
  } catch (err) {
    console.error('not ok - ' + name);
    throw err;
  }
}

run('fileTreeRefreshKeyFromItems: 非数组返回空签名', () => {
  assert.equal(fileTreeRefreshKeyFromItems(null), '');
  assert.equal(fileTreeRefreshKeyFromItems({}), '');
});

run('fileTreeRefreshKeyFromItems: 忽略未完成工具', () => {
  const key = fileTreeRefreshKeyFromItems([
    { kind: 'tool', id: 1, tool: { isDone: false, tool: 'file_write', title: 'a.txt' } },
    { kind: 'msg', id: 2, role: 'assistant', content: 'ok' },
  ]);
  assert.equal(key, '');
});

run('fileTreeRefreshKeyFromItems: 已完成工具生成稳定签名', () => {
  const key = fileTreeRefreshKeyFromItems([
    {
      kind: 'tool',
      id: 3,
      tool: {
        isDone: true,
        success: true,
        tool: 'file_write',
        toolCallId: 'call-1',
        summary: { object: 'openspec/tasks.md' },
        hunks: [{ file: 'openspec/tasks.md' }],
      },
    },
  ]);
  assert.equal(key, '3:file_write:call-1:1:openspec/tasks.md:1');
});

run('fileTreeRefreshKeyFromItems: 失败工具也会触发刷新机会', () => {
  const key = fileTreeRefreshKeyFromItems([
    {
      kind: 'tool',
      id: 4,
      tool: {
        isDone: true,
        success: false,
        tool: 'bash',
        toolCallId: 'call-2',
        title: 'mkdir src',
        hunks: [],
      },
    },
  ]);
  assert.equal(key, '4:bash:call-2:0:mkdir src:0');
});

run('fileTreeReloadPaths: 无展开目录时只刷新根目录', () => {
  assert.deepEqual(fileTreeReloadPaths(null), ['']);
  assert.deepEqual(fileTreeReloadPaths({}), ['']);
  assert.deepEqual(fileTreeReloadPaths(new Set()), ['']);
});

run('fileTreeReloadPaths: 根目录优先,随后按展开顺序刷新子目录', () => {
  const paths = fileTreeReloadPaths(new Set(['src', 'src/session', 'web']));
  assert.deepEqual(paths, ['', 'src', 'src/session', 'web']);
});

run('fileTreeReloadPaths: 去重空路径和重复展开目录', () => {
  const paths = fileTreeReloadPaths(['', 'src', 'src', 'src/tool']);
  assert.deepEqual(paths, ['', 'src', 'src/tool']);
});

run('fileTreeDirectoryEntriesEqual: 只比较影响可见行的有序字段', () => {
  const current = [
    { name: 'components', path: 'src/components', kind: 'dir', modified_ms: 100 },
    { name: 'main.cpp', path: 'src/main.cpp', kind: 'file', size: 10, modified_ms: 100 },
  ];
  const incoming = [
    { name: 'components', path: 'src/components', kind: 'dir', modified_ms: 200 },
    { name: 'main.cpp', path: 'src/main.cpp', kind: 'file', size: 20, modified_ms: 200 },
  ];

  assert.equal(fileTreeDirectoryEntriesEqual(current, incoming), true);
  assert.equal(fileTreeDirectoryEntriesEqual(current, incoming.slice().reverse()), false);
});

run('reconcileFileTreeDirectory: 首次缓存空目录仍创建目录条目', () => {
  const current = new Map();
  const next = reconcileFileTreeDirectory(current, 'empty', []);

  assert.notEqual(next, current);
  assert.equal(next.has('empty'), true);
  assert.deepEqual(next.get('empty'), []);
});

run('reconcileFileTreeDirectory: 相同克隆响应保留 Map 和行引用', () => {
  const rows = [
    { name: 'lib', path: 'src/lib', kind: 'dir', modified_ms: 100 },
    { name: 'main.cpp', path: 'src/main.cpp', kind: 'file', size: 10 },
  ];
  const current = new Map([['src', rows]]);
  const incoming = rows.map((row) => ({ ...row, modified_ms: 999, size: 999 }));
  const next = reconcileFileTreeDirectory(current, 'src', incoming);

  assert.equal(next, current);
  assert.equal(next.get('src'), rows);
});

run('reconcileFileTreeDirectory: 可见目录结构变化只替换目标目录', () => {
  const rootRows = [{ name: 'src', path: 'src', kind: 'dir' }];
  const srcRows = [{ name: 'main.cpp', path: 'src/main.cpp', kind: 'file' }];
  const current = new Map([
    ['', rootRows],
    ['src', srcRows],
  ]);
  const incoming = [
    { name: 'lib', path: 'src/lib', kind: 'dir' },
    { name: 'main.cpp', path: 'src/main.cpp', kind: 'file' },
  ];
  const next = reconcileFileTreeDirectory(current, 'src', incoming);

  assert.notEqual(next, current);
  assert.equal(next.get(''), rootRows);
  assert.equal(next.get('src'), incoming);
});

run('reconcileFileTreeDirectory: 重命名和 kind 变化会更新缓存', () => {
  const current = new Map([[
    'src',
    [{ name: 'main.cpp', path: 'src/main.cpp', kind: 'file' }],
  ]]);

  const renamed = reconcileFileTreeDirectory(current, 'src', [
    { name: 'app.cpp', path: 'src/app.cpp', kind: 'file' },
  ]);
  const kindChanged = reconcileFileTreeDirectory(current, 'src', [
    { name: 'main.cpp', path: 'src/main.cpp', kind: 'dir' },
  ]);

  assert.notEqual(renamed, current);
  assert.notEqual(kindChanged, current);
});

run('fileTreeDirectoryRequestKey: cwd 和目录边界不会碰撞', () => {
  const first = fileTreeDirectoryRequestKey('/repo', 'src/lib');
  const same = fileTreeDirectoryRequestKey('/repo', 'src/lib');
  const otherPath = fileTreeDirectoryRequestKey('/repo/src', 'lib');
  const otherCwd = fileTreeDirectoryRequestKey('/other', 'src/lib');

  assert.equal(first, same);
  assert.notEqual(first, otherPath);
  assert.notEqual(first, otherCwd);
});

run('beginFileTreeDirectoryRequest: 同 cwd/path 请求同步去重并可释放', () => {
  const inFlight = new Set();
  const first = beginFileTreeDirectoryRequest(inFlight, '/repo', 'src');
  const duplicate = beginFileTreeDirectoryRequest(inFlight, '/repo', 'src');
  const otherCwd = beginFileTreeDirectoryRequest(inFlight, '/other', 'src');

  assert.equal(first, fileTreeDirectoryRequestKey('/repo', 'src'));
  assert.equal(duplicate, null);
  assert.equal(otherCwd, fileTreeDirectoryRequestKey('/other', 'src'));

  finishFileTreeDirectoryRequest(inFlight, first);
  assert.equal(beginFileTreeDirectoryRequest(inFlight, '/repo', 'src'), first);
});
