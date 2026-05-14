// mergeAllWorkspaceSessions 单元测试。
//
// 该函数是 SearchPalette 的数据入口:跨所有 workspace 拉 sessions、注入 workspace
// 名 / hash、单 workspace 失败不阻塞其它 workspace。这里只测纯合并/错误收集逻辑,
// 不打真实网络,通过依赖注入 listWorkspaces / listSessions 两个 mock 完成。

import assert from 'node:assert/strict';
import { ApiError, createApi, mergeAllWorkspaceSessions } from './api.js';

function run(name, fn) {
  try {
    const ret = fn();
    if (ret && typeof ret.then === 'function') {
      return ret.then(
        () => console.log(`[pass] ${name}`),
        (err) => { console.error(`[fail] ${name}`); throw err; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('合并多个 workspace 的 sessions 并注入 workspaceName', async () => {
  const ws = [
    { hash: 'h1', name: 'acecode', cwd: '/acecode' },
    { hash: 'h2', name: 'hermes',  cwd: '/hermes' },
  ];
  const sessionsByHash = {
    h1: [
      { id: 's1', title: 'Hello',  updated_at: '2026-05-01' },
      { id: 's2', title: 'World',  updated_at: '2026-05-02' },
    ],
    h2: [
      { id: 's3', title: 'Bonjour', updated_at: '2026-05-03', workspace_hash: 'h2' },
    ],
  };
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions:   async (hash) => sessionsByHash[hash],
  });
  assert.equal(result.sessions.length, 3);
  assert.equal(result.errors.length, 0);
  assert.equal(result.sessions[0].workspaceName, 'acecode');
  assert.equal(result.sessions[0].workspace_hash, 'h1');
  assert.equal(result.sessions[2].workspaceName, 'hermes');
  // 已有 workspace_hash 字段不被覆盖
  assert.equal(result.sessions[2].workspace_hash, 'h2');
});

await run('单个 workspace 拉取失败,其它 workspace 正常返回', async () => {
  const ws = [
    { hash: 'h1', name: 'good', cwd: '/good' },
    { hash: 'h2', name: 'bad',  cwd: '/bad' },
    { hash: 'h3', name: 'also-good', cwd: '/og' },
  ];
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions:   async (hash) => {
      if (hash === 'h2') throw new Error('500 oops');
      return [{ id: 's-' + hash, title: 't' }];
    },
  });
  assert.equal(result.sessions.length, 2);
  assert.equal(result.errors.length, 1);
  assert.equal(result.errors[0].hash, 'h2');
  assert.equal(result.errors[0].name, 'bad');
  assert.match(result.errors[0].message, /500 oops/);
});

await run('listWorkspaces 失败时返回空数组并把错误归档', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => { throw new Error('auth fail'); },
    listSessions:   async () => { throw new Error('should not call'); },
  });
  assert.deepEqual(result.sessions, []);
  assert.equal(result.errors.length, 1);
  assert.match(result.errors[0].message, /auth fail/);
});

await run('空 workspace 列表返回 empty result', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => [],
    listSessions:   async () => { throw new Error('should not call'); },
  });
  assert.deepEqual(result.sessions, []);
  assert.deepEqual(result.errors, []);
});

await run('listWorkspaces 返回非数组 → 视为空 + 不报错', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => null,
    listSessions:   async () => [],
  });
  assert.deepEqual(result.sessions, []);
  assert.deepEqual(result.errors, []);
});

await run('listSessions 返回非数组 → 视为空 + 不报错', async () => {
  const ws = [{ hash: 'h1', name: 'x' }];
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions:   async () => null,
  });
  assert.equal(result.sessions.length, 0);
  assert.equal(result.errors.length, 0);
});

await run('executeCommand posts to builtin command endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 202,
      headers: { get: () => 'application/json' },
      json: async () => ({ queued: true, command: 'init' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.executeCommand('session/a', {
      command: 'init',
      args: '',
      display_text: '/init',
    });
    assert.deepEqual(result, { queued: true, command: 'init' });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/session%2Fa/commands');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), {
      command: 'init',
      args: '',
      display_text: '/init',
    });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('readFileBlob fetches authenticated binary file content', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'image/png' },
      blob: async () => new Blob(['png-bytes'], { type: 'image/png' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const blob = await client.readFileBlob('/repo root', 'docs/a b.png');

    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/files/blob?cwd=%2Frepo%20root&path=docs%2Fa%20b.png');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(blob.type, 'image/png');
    assert.equal(await blob.text(), 'png-bytes');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('readFileBlob parses JSON errors as ApiError body', async () => {
  const previousFetch = globalThis.fetch;
  globalThis.fetch = async () => ({
    ok: false,
    status: 415,
    headers: { get: () => 'application/json' },
    json: async () => ({ error: 'file too large', size: 123 }),
  });
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await assert.rejects(
      () => client.readFileBlob('/repo', 'large.png'),
      (err) => {
        assert.equal(err instanceof ApiError, true);
        assert.equal(err.status, 415);
        assert.deepEqual(err.body, { error: 'file too large', size: 123 });
        return true;
      },
    );
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('archive API methods use expected endpoints and archived query flag', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => [],
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.listSessions({ archived: true });
    await client.listWorkspaceSessions('w/a', { archived: true });
    await client.archiveSession('s/a');
    await client.unarchiveWorkspaceSession('w/a', 's/a');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions?archived=1');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions?archived=1');
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/archive');
    assert.equal(calls[2].opts.method, 'PUT');
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/archive');
    assert.equal(calls[3].opts.method, 'DELETE');
  } finally {
    globalThis.fetch = previousFetch;
  }
});
