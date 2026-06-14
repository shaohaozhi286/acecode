import assert from 'node:assert/strict';
import { fileTreeRefreshKeyFromItems, fileTreeReloadPaths } from './fileTreeRefresh.js';

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
