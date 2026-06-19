// 首页 workspace 选择逻辑回归测试。

import assert from 'node:assert/strict';
import {
  DEFAULT_HOME_WORKSPACE_SELECTION,
  homeWorkspaceOptionForHash,
  noHomeWorkspaceOption,
  readDesktopHomeWorkspaceHash,
  resolveHomeWorkspaceHash,
  validateHomeWorkspaceSelection,
  writeDesktopHomeWorkspaceHash,
} from './homeWorkspaceSelection.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const options = [
  { hash: 'ws-a', name: 'aaa', cwd: 'C:/aaa' },
  { hash: 'ws-b', name: 'bbb', cwd: 'C:/bbb' },
];

await run('默认偏好为空 hash,表示不使用工作区', () => {
  assert.deepEqual(DEFAULT_HOME_WORKSPACE_SELECTION, { workspaceHash: '' });
  assert.equal(validateHomeWorkspaceSelection(DEFAULT_HOME_WORKSPACE_SELECTION), true);
  assert.equal(resolveHomeWorkspaceHash({ options }), '');
  assert.equal(homeWorkspaceOptionForHash(options, '').noWorkspace, true);
});

await run('已保存 workspace 存在时优先使用保存值', () => {
  assert.equal(resolveHomeWorkspaceHash({
    preferredHash: 'ws-b',
    explicitHash: '',
    previousHash: '',
    options,
  }), 'ws-b');
});

await run('显式打开某 workspace 时优先于保存值', () => {
  assert.equal(resolveHomeWorkspaceHash({
    preferredHash: 'ws-b',
    explicitHash: 'ws-a',
    previousHash: '',
    options,
  }), 'ws-a');
});

await run('保存的 workspace 不存在时回到不使用工作区', () => {
  assert.equal(resolveHomeWorkspaceHash({
    preferredHash: 'missing',
    explicitHash: '',
    previousHash: '',
    options,
  }), '');
});

await run('按 hash 解析选项,空 hash 返回不使用工作区选项', () => {
  assert.equal(homeWorkspaceOptionForHash(options, 'ws-a').name, 'aaa');
  assert.deepEqual(noHomeWorkspaceOption(), homeWorkspaceOptionForHash(options, ''));
});

await run('desktop bridge 读写 hash,兼容字符串 JSON 返回', async () => {
  const calls = [];
  const win = {
    aceDesktop_getHomeWorkspaceHash: async () => '{"workspace_hash":"ws-a"}',
    aceDesktop_setHomeWorkspaceHash: async (hash) => {
      calls.push(hash);
      return '{"ok":true}';
    },
  };
  assert.equal(await readDesktopHomeWorkspaceHash(win), 'ws-a');
  assert.equal(await writeDesktopHomeWorkspaceHash('ws-b', win), true);
  assert.deepEqual(calls, ['ws-b']);
});

await run('desktop bridge 不可用时返回 fallback 结果', async () => {
  assert.equal(await readDesktopHomeWorkspaceHash({}), null);
  assert.equal(await writeDesktopHomeWorkspaceHash('ws-a', {}), false);
});
