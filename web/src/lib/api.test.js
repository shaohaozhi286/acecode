// mergeAllWorkspaceSessions 单元测试。
//
// 该函数是 SearchPalette 的数据入口:跨所有 workspace 拉 sessions、注入 workspace
// 名 / hash、单 workspace 失败不阻塞其它 workspace。这里只测纯合并/错误收集逻辑,
// 不打真实网络,通过依赖注入 listWorkspaces / listSessions 两个 mock 完成。

import assert from 'node:assert/strict';
import { ApiError, createApi, mergeAllWorkspaceSessions, sessionDraftPath, sessionTodosPath } from './api.js';

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

await run('getUsageStats uses usage endpoint query params and auth token', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ summary: { records: 0 } }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getUsageStats({ days: 7, workspace: 'w/a', timezoneOffsetMinutes: -480 });

    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/usage?days=7&workspace=w%2Fa&timezone_offset_minutes=-480');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
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

await run('probeModels posts draft to model probe endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ models: ['gpt-4o'] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.probeModels({
      provider: 'openai',
      base_url: 'http://localhost:1234/v1',
      api_key: 'sk',
    });
    assert.deepEqual(result, { models: ['gpt-4o'] });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/models/probe');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), {
      provider: 'openai',
      base_url: 'http://localhost:1234/v1',
      api_key: 'sk',
    });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Copilot auth API methods use expected endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true, status: 'pending' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getCopilotAuth();
    await client.startCopilotAuth();
    await client.pollCopilotAuth('device-123');
    await client.logoutCopilot();

    assert.equal(calls.length, 4);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/copilot/auth');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/copilot/auth/device');
    assert.equal(calls[1].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {});
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/copilot/auth/device/poll');
    assert.equal(calls[2].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[2].opts.body), { device_code: 'device-123' });
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/copilot/auth');
    assert.equal(calls[3].opts.method, 'DELETE');
    for (const call of calls) {
      assert.equal(call.opts.headers['X-ACECode-Token'], 'tok');
    }
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('UI preference API keeps legacy avatar preference endpoint compatible', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ show_acecode_avatar: false }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getUiPreferences();
    const saved = await client.setUiPreferences({ show_acecode_avatar: false });

    assert.deepEqual(got, { show_acecode_avatar: false });
    assert.deepEqual(saved, { show_acecode_avatar: false });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/ui-preferences');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/ui-preferences');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { show_acecode_avatar: false });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Upgrade config API reads and writes daemon-backed base_url', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ base_url: 'http://2017studio.imwork.net:82/aupdate/' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getUpgradeConfig();
    const saved = await client.setUpgradeConfig({ base_url: 'https://updates.example.test/ace' });

    assert.deepEqual(got, { base_url: 'http://2017studio.imwork.net:82/aupdate/' });
    assert.deepEqual(saved, { base_url: 'http://2017studio.imwork.net:82/aupdate/' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/upgrade');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/upgrade');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {
      base_url: 'https://updates.example.test/ace',
    });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Update API checks status and starts explicit update action', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: opts.method === 'POST' ? 202 : 200,
      headers: { get: () => 'application/json' },
      json: async () => opts.method === 'POST'
        ? ({ started: true, latest_version: '9.9.9' })
        : ({ status: 'available', update_available: true, latest_version: '9.9.9' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const status = await client.getUpdateStatus();
    const started = await client.startUpdate();

    assert.deepEqual(status, {
      status: 'available',
      update_available: true,
      latest_version: '9.9.9',
    });
    assert.deepEqual(started, { started: true, latest_version: '9.9.9' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/update/status');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/update/start');
    assert.equal(calls[1].opts.method, 'POST');
    assert.equal(calls[1].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('ACE Browser Bridge API reads and writes daemon-backed enabled flag', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ enabled: true, tool_mode: 'progressive' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getAceBrowserBridge();
    const saved = await client.setAceBrowserBridge({ enabled: true });

    assert.equal(got.enabled, true);
    assert.equal(saved.tool_mode, 'progressive');
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/ace-browser-bridge');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/ace-browser-bridge');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { enabled: true });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('getSkillRoot uses workspace query and auth token', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ path: '/repo/.acecode/skills', source: 'project_acecode' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getSkillRoot('workspace/hash');

    assert.deepEqual(got, { path: '/repo/.acecode/skills', source: 'project_acecode' });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/skills/root?workspace=workspace%2Fhash');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
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

await run('session draft API uses workspace route when workspace hash is available', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ session_id: 's/a', text: 'draft text' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    assert.equal(
      sessionDraftPath('s/a', 'w/a'),
      '/api/workspaces/w%2Fa/sessions/s%2Fa/draft',
    );
    assert.equal(sessionDraftPath('s/a'), '/api/sessions/s%2Fa/draft');

    await client.getSessionDraft('s/a', 'w/a');
    await client.setSessionDraft('s/a', 'draft text', 'w/a');
    await client.setSessionDraft('s/a', '');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/draft');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/draft');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { text: 'draft text' });
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/draft');
    assert.equal(calls[2].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[2].opts.body), { text: '' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('session todos API clears through workspace route when available', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ session_id: 's/a', todos: [] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    assert.equal(
      sessionTodosPath('s/a', 'w/a'),
      '/api/workspaces/w%2Fa/sessions/s%2Fa/todos',
    );
    assert.equal(sessionTodosPath('s/a'), '/api/sessions/s%2Fa/todos');

    await client.clearSessionTodos('s/a', 'w/a');
    await client.clearSessionTodos('s/a');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/todos');
    assert.equal(calls[0].opts.method, 'DELETE');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/todos');
    assert.equal(calls[1].opts.method, 'DELETE');
  } finally {
    globalThis.fetch = previousFetch;
  }
});
