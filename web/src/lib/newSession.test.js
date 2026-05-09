// newSession.js 单测。
//
// 托盘 "新建会话" 要真实 POST 创建 session,不能只切到空态首页。

import assert from 'node:assert/strict';
import {
  createNewSessionForActiveWorkspace,
  sessionRefFromCreateResponse,
} from './newSession.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('createNewSessionForActiveWorkspace 有 workspaceHash 时创建 workspace session', async () => {
  const calls = [];
  const api = {
    createSession: async () => {
      calls.push(['createSession']);
      return { session_id: 'wrong' };
    },
    createWorkspaceSession: async (hash, opts) => {
      calls.push(['createWorkspaceSession', hash, opts]);
      return { session_id: 's1', workspace_hash: hash, cwd: 'C:/repo', created_at: '2026-05-08T01:00:00Z' };
    },
    listWorkspaceSessions: async (hash) => {
      calls.push(['listWorkspaceSessions', hash]);
      return [
        { id: 'old', workspace_hash: hash, created_at: '2026-05-08T00:00:00Z' },
        { id: 's1', workspace_hash: hash, created_at: '2026-05-08T01:00:00Z' },
      ];
    },
  };

  const ref = await createNewSessionForActiveWorkspace(api, {
    workspaceHash: 'w1',
    port: 1234,
    token: 'tok',
  }, { cwd: 'fallback' });

  assert.deepEqual(calls, [
    ['createWorkspaceSession', 'w1', {}],
    ['listWorkspaceSessions', 'w1'],
  ]);
  assert.equal(ref.sessionId, 's1');
  assert.equal(ref.workspaceHash, 'w1');
  assert.equal(ref.cwd, 'C:/repo');
  assert.equal(ref.port, 1234);
  assert.equal(ref.token, 'tok');
  assert.equal(ref.contextId, 'default');
  assert.equal(ref.displayTitle, '新会话2');
});

await run('createNewSessionForActiveWorkspace 无真实 workspace 时创建兼容 session', async () => {
  const calls = [];
  const api = {
    createSession: async () => {
      calls.push(['createSession']);
      return { id: 's2', workspace_hash: 'compat', cwd: 'C:/compat', created_at: '2026-05-08T01:00:00Z' };
    },
    createWorkspaceSession: async (hash) => {
      calls.push(['createWorkspaceSession', hash]);
      return { session_id: 'wrong' };
    },
    listWorkspaceSessions: async (hash) => {
      calls.push(['listWorkspaceSessions', hash]);
      return [{ id: 's2', workspace_hash: hash, created_at: '2026-05-08T01:00:00Z' }];
    },
  };

  const ref = await createNewSessionForActiveWorkspace(api, { workspaceHash: '__local__' }, null);

  assert.deepEqual(calls, [
    ['createSession'],
    ['listWorkspaceSessions', 'compat'],
  ]);
  assert.equal(ref.sessionId, 's2');
  assert.equal(ref.workspaceHash, 'compat');
  assert.equal(ref.cwd, 'C:/compat');
  assert.equal(ref.displayTitle, '新会话1');
});

await run('sessionRefFromCreateResponse 缺 session id 时抛错', () => {
  assert.throws(
    () => sessionRefFromCreateResponse({ workspace_hash: 'w1' }, {}, null),
    /session_id/,
  );
});
