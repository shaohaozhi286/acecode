// Slash command workspace 选择逻辑回归测试。

import assert from 'node:assert/strict';
import { commandWorkspaceHashForInput } from './slashCommandWorkspace.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('无 session 时使用首页项目选择器的 workspace', () => {
  const hash = commandWorkspaceHashForInput({
    activeRef: null,
    selectedHomeWorkspace: { hash: 'ws-ttt' },
    hasSession: false,
  });
  assert.equal(hash, 'ws-ttt');
});

run('无 session 且选择器尚未加载时回退 activeRef workspace', () => {
  const hash = commandWorkspaceHashForInput({
    activeRef: { workspaceHash: 'ws-active' },
    selectedHomeWorkspace: null,
    hasSession: false,
  });
  assert.equal(hash, 'ws-active');
});

run('无 session 且显式选择不使用工作区时不回退 activeRef workspace', () => {
  const hash = commandWorkspaceHashForInput({
    activeRef: { workspaceHash: 'ws-active' },
    selectedHomeWorkspace: { hash: '', noWorkspace: true },
    hasSession: false,
  });
  assert.equal(hash, '');
});

run('有 session 时锁定会话 workspace,避免被首页选择器旧状态污染', () => {
  const hash = commandWorkspaceHashForInput({
    activeRef: { workspaceHash: 'ws-session' },
    selectedHomeWorkspace: { hash: 'ws-home' },
    hasSession: true,
  });
  assert.equal(hash, 'ws-session');
});
