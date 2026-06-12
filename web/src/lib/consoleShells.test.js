// consoleShells.js 单测(plan: 控制台 Shell 选择器)。
// 覆盖:响应归一化(label 兜底 / needs_path 映射 / 过滤坏项 / 默认 id)、
// 下拉项构造(isDefault 标注)、按 id 查找。

import assert from 'node:assert/strict';
import { normalizeShells, buildShellMenuItems, shellById } from './consoleShells.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 触发场景:后端正常响应(含 git-bash needs_path)。
// 期望:字段映射正确,needs_path → needsPath,缺 label 用 id 兜底,默认 id 透出。
run('normalizeShells maps fields and falls back label to id', () => {
  const out = normalizeShells({
    shells: [
      { id: 'powershell', label: 'PowerShell 7', available: true },
      { id: 'git-bash', label: 'Git Bash', available: false, needs_path: true },
      { id: 'cmd' }, // 无 label / available
    ],
    default: 'cmd',
  });
  assert.equal(out.defaultId, 'cmd');
  assert.equal(out.shells.length, 3);
  assert.deepEqual(out.shells[1], { id: 'git-bash', label: 'Git Bash', available: false, needsPath: true });
  // cmd 缺字段:label 兜底为 id,available 缺省 true,needsPath false
  assert.deepEqual(out.shells[2], { id: 'cmd', label: 'cmd', available: true, needsPath: false });
});

// 触发场景:响应里混入坏项(无 id / null)。期望:被过滤掉,不抛。
run('normalizeShells filters invalid entries', () => {
  const out = normalizeShells({ shells: [null, {}, { id: '' }, { id: 'cmd' }], default: 7 });
  assert.equal(out.shells.length, 1);
  assert.equal(out.shells[0].id, 'cmd');
  assert.equal(out.defaultId, ''); // 非字符串 default → 空
});

// 触发场景:完全空 / 非对象响应。期望:返回空列表,不抛。
run('normalizeShells tolerates empty response', () => {
  assert.deepEqual(normalizeShells(undefined), { shells: [], defaultId: '' });
  assert.deepEqual(normalizeShells({}), { shells: [], defaultId: '' });
});

// 触发场景:构造下拉项,默认是 git-bash。期望:对应项 isDefault=true,其余 false。
run('buildShellMenuItems marks the default item', () => {
  const { shells } = normalizeShells({
    shells: [{ id: 'powershell', label: 'PowerShell' }, { id: 'git-bash', label: 'Git Bash', needs_path: true }],
    default: 'git-bash',
  });
  const items = buildShellMenuItems(shells, 'git-bash');
  assert.equal(items.find((i) => i.id === 'powershell').isDefault, false);
  const gb = items.find((i) => i.id === 'git-bash');
  assert.equal(gb.isDefault, true);
  assert.equal(gb.needsPath, true);
});

// 触发场景:按 id 查找命中 / 未命中。期望:命中返回该项,未命中返回 null。
run('shellById finds or returns null', () => {
  const shells = [{ id: 'cmd' }, { id: 'powershell' }];
  assert.equal(shellById(shells, 'powershell').id, 'powershell');
  assert.equal(shellById(shells, 'nope'), null);
  assert.equal(shellById(null, 'cmd'), null);
});
