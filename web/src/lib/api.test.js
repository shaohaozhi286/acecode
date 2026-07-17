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
  assert.deepEqual(result.workspaces, ws);
  // 已有 workspace_hash 字段不被覆盖
  assert.equal(result.sessions[2].workspace_hash, 'h2');
});

await run('补入 no_workspace 活跃会话并过滤兼容列表中的普通会话', async () => {
  const ws = [{ hash: 'h1', name: 'acecode', cwd: '/acecode' }];
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions: async () => [{ id: 's1', title: 'Workspace task' }],
    listNoWorkspaceSessions: async () => [
      { id: 's1', title: 'Compatibility duplicate', workspace_hash: 'h1' },
      { id: 'temp-1', no_workspace: true, active: true },
    ],
  });

  assert.deepEqual(result.sessions.map((session) => session.id), ['s1', 'temp-1']);
  assert.deepEqual(result.workspaces, ws);
  const temporary = result.sessions[1];
  assert.equal(temporary.no_workspace, true);
  assert.equal(temporary.workspace_hash, '');
  assert.equal(temporary.workspaceName, '无工作区');
  assert.equal(temporary.cwd, '');
});

await run('no_workspace 补充请求失败不丢弃 workspace 结果', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => [{ hash: 'h1', name: 'acecode', cwd: '/acecode' }],
    listSessions: async () => [{ id: 's1', title: 'Still visible' }],
    listNoWorkspaceSessions: async () => { throw new Error('temporary list failed'); },
  });

  assert.deepEqual(result.sessions.map((session) => session.id), ['s1']);
  assert.equal(result.errors.length, 1);
  assert.equal(result.errors[0].name, '无工作区');
  assert.match(result.errors[0].message, /temporary list failed/);
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

await run('searchSessionUserMessages uses bounded content search endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ matches: [{ id: 's1' }] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.searchSessionUserMessages('sqlite 索引', 200);

    assert.deepEqual(result, { matches: [{ id: 's1' }] });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/session-search/user-messages?q=sqlite+%E7%B4%A2%E5%BC%95&limit=100');
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

await run('askSideQuestion posts the isolated question payload', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ question: 'why?', answer: 'because' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.askSideQuestion('session/a', 'why?');
    assert.deepEqual(result, { question: 'why?', answer: 'because' });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/session%2Fa/side-question');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { question: 'why?' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('opencode import API uses workspace-scoped endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true, job_id: 'job/1' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getOpencodeImportPreview('w/a');
    await client.startOpencodeImport('w/a', ['ses/1', 'ses-2']);
    await client.getOpencodeImportJob('w/a', 'job/1');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/opencode-import');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/opencode-import');
    assert.equal(calls[1].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { session_ids: ['ses/1', 'ses-2'] });
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/opencode-import/job%2F1');
    assert.equal(calls[2].opts.method, 'GET');
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
      request_headers: { 'X-Probe': 'acecode' },
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
      request_headers: { 'X-Probe': 'acecode' },
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

await run('Desktop onboarding API reads status and persists dismissal', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ guide_version: 1, dismissed: calls.length > 1 }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const status = await client.getDesktopOnboarding();
    const dismissed = await client.dismissDesktopOnboarding();

    assert.deepEqual(status, { guide_version: 1, dismissed: false });
    assert.deepEqual(dismissed, { guide_version: 1, dismissed: true });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/ui/onboarding/desktop');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/ui/onboarding/desktop/dismiss');
    assert.equal(calls[1].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Custom instructions API reads and writes daemon-backed text', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ text: 'Prefer Chinese replies.' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getCustomInstructions();
    const saved = await client.setCustomInstructions({ text: 'Prefer Chinese replies.' });

    assert.deepEqual(got, { text: 'Prefer Chinese replies.' });
    assert.deepEqual(saved, { text: 'Prefer Chinese replies.' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/custom-instructions');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/custom-instructions');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { text: 'Prefer Chinese replies.' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Connectors API reads and writes daemon-backed connector list', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  const payload = {
    connectors: [
      {
        id: 'alpha-connector',
        name: 'Alpha Connector',
        description: 'Connect alpha providers',
        enabled: true,
      },
    ],
  };
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => payload,
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getConnectors();
    const saved = await client.setConnectors(payload);

    assert.deepEqual(got, payload);
    assert.deepEqual(saved, payload);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/connectors');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/connectors');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), payload);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Pinned session visual order API reads and writes global order endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ items: [{ workspace_hash: 'w1', session_id: 'a' }] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getPinnedSessionOrder();
    const saved = await client.setPinnedSessionOrder([{ workspace_hash: 'w2', session_id: 'b' }]);

    assert.deepEqual(got, { items: [{ workspace_hash: 'w1', session_id: 'a' }] });
    assert.deepEqual(saved, { items: [{ workspace_hash: 'w1', session_id: 'a' }] });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/pinned-sessions/order');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/pinned-sessions/order');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {
      items: [{ workspace_hash: 'w2', session_id: 'b' }],
    });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Default permission mode API reads and writes config endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ mode: 'accept-edits' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getDefaultPermissionMode();
    const saved = await client.setDefaultPermissionMode('accept-edits');

    assert.deepEqual(got, { mode: 'accept-edits' });
    assert.deepEqual(saved, { mode: 'accept-edits' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/default-permission-mode');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/default-permission-mode');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { mode: 'accept-edits' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Desktop notification API reads and writes the master switch', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ enabled: false }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getDesktopNotifications();
    await client.setDesktopNotifications(false);

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/desktop-notifications');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/desktop-notifications');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { enabled: false });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Session model switch and default model use separate endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ name: 'fast' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.switchModel('sid-1', 'fast');
    await client.setDefaultModel('slow');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/sid-1/model');
    assert.equal(calls[0].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { name: 'fast' });
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/default-model');
    assert.equal(calls[1].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { name: 'slow' });
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

await run('Update API checks status and manages GUI update jobs', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: opts.method === 'POST' ? 202 : 200,
      headers: { get: () => 'application/json' },
      json: async () => {
        if (url.endsWith('/api/update/status')) {
          return { status: 'available', update_available: true, latest_version: '9.9.9' };
        }
        if (opts.method === 'POST') return { started: true, job_id: 'job-1', target_version: '9.9.9' };
        return { job_id: 'job-1', state: 'running', phase: 'downloading' };
      },
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const status = await client.getUpdateStatus();
    const started = await client.startUpdate();
    const latest = await client.getLatestUpdateJob();
    const job = await client.getUpdateJob('job 1');

    assert.deepEqual(status, {
      status: 'available',
      update_available: true,
      latest_version: '9.9.9',
    });
    assert.deepEqual(started, { started: true, job_id: 'job-1', target_version: '9.9.9' });
    assert.deepEqual(latest, { job_id: 'job-1', state: 'running', phase: 'downloading' });
    assert.deepEqual(job, { job_id: 'job-1', state: 'running', phase: 'downloading' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/update/status');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/update/start');
    assert.equal(calls[1].opts.method, 'POST');
    assert.equal(calls[1].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/update/job');
    assert.equal(calls[2].opts.method, 'GET');
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/update/jobs/job%201');
    assert.equal(calls[3].opts.method, 'GET');
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

await run('Desktop feedback API methods use dedicated feedback endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => (
        url.includes('recent-sessions')
          ? { sessions: [{ id: 's1' }] }
          : { ok: true, package_filename: 'feedback.zip' }
      ),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const sessions = await client.listDesktopFeedbackSessions(25);
    const submitted = await client.submitDesktopFeedback({
      feedback_text: 'desktop issue',
      session_id: 's1',
      workspace_hash: 'w1',
    });

    assert.deepEqual(sessions, { sessions: [{ id: 's1' }] });
    assert.deepEqual(submitted, { ok: true, package_filename: 'feedback.zip' });
    assert.equal(calls.length, 2);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/feedback/desktop/recent-sessions?limit=25');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/feedback/desktop');
    assert.equal(calls[1].opts.method, 'POST');
    assert.equal(calls[1].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {
      feedback_text: 'desktop issue',
      session_id: 's1',
      workspace_hash: 'w1',
    });
    assert.ok(!calls.some((call) => call.url.includes('/messages') || call.url.includes('/commands')));
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

await run('listCommands distinguishes explicit no-workspace from omitted workspace', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ builtins: [], commands: [], skills: [] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.listCommands('');
    await client.listCommands();

    assert.equal(calls.length, 2);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/commands?workspace=');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/commands');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Hook management API methods use encoded hook ids and expected endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ hooks: [], sources: [] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const hookId = 'project_local:codex_json:C:/repo/.codex/hooks.json::PreToolUse#1.1';
    await client.listHooks();
    await client.refreshHooks();
    await client.trustHook(hookId);
    await client.disableHook(hookId);
    await client.enableHook(hookId);

    assert.equal(calls.length, 5);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/hooks');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/hooks/refresh');
    assert.equal(calls[1].opts.method, 'POST');
    const encoded = encodeURIComponent(hookId);
    assert.equal(calls[2].url, `http://127.0.0.1:4567/api/hooks/${encoded}/trust`);
    assert.equal(calls[2].opts.method, 'POST');
    assert.equal(calls[3].url, `http://127.0.0.1:4567/api/hooks/${encoded}/disable`);
    assert.equal(calls[3].opts.method, 'POST');
    assert.equal(calls[4].url, `http://127.0.0.1:4567/api/hooks/${encoded}/enable`);
    assert.equal(calls[4].opts.method, 'POST');
    for (const call of calls) {
      assert.equal(call.opts.headers['X-ACECode-Token'], 'tok');
    }
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

await run('exportSession posts the selected session and workspace identity', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true, filename: '会话.md' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.exportSession('s/1', 'workspace-1');
    assert.deepEqual(result, { ok: true, filename: '会话.md' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/s%2F1/export-markdown');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { workspace_hash: 'workspace-1' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('listFiles can request hidden and noise entries for path references', async () => {
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
    await client.listFiles('/repo root', 'src/deep', true, true);
    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/files?cwd=%2Frepo%20root&path=src%2Fdeep&show_hidden=1&show_noise=1',
    );
    assert.equal(calls[0].opts.method, 'GET');
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
    await client.purgeArchivedSession('s/a');
    await client.purgeArchivedWorkspaceSession('w/a', 's/a');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions?archived=1');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions?archived=1');
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/archive');
    assert.equal(calls[2].opts.method, 'PUT');
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/archive');
    assert.equal(calls[3].opts.method, 'DELETE');
    assert.equal(calls[4].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa?purge=1');
    assert.equal(calls[4].opts.method, 'DELETE');
    assert.equal(calls[5].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa?purge=1');
    assert.equal(calls[5].opts.method, 'DELETE');
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
