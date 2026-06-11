// HTTP 客户端封装:自动带 X-ACECode-Token,JSON 编解码,错误抛 ApiError。
//
// 共享 daemon 模型: workspace 是 API/session 数据,不是 daemon 端口归属。
// setBase({port, token}) 仍保留给 standalone/desktop bootstrap 与兼容场景。

import { getToken } from './auth.js';

export class ApiError extends Error {
  constructor(status, body) {
    // 后端 4xx/5xx 通常返回 {error: "<CODE>", message: "<human msg>"}。
    // 把 error 抽出到 .code,把 message 抽出到 .message,前端可走 errors.js 做 i18n;
    // 拿不到结构化字段时退到原 JSON.stringify(body) 兜底,保留旧行为。
    const code = body && typeof body === 'object' ? body.error : undefined;
    const friendly = body && typeof body === 'object' ? body.message : undefined;
    const text = friendly || (typeof body === 'string' ? body : JSON.stringify(body));
    super(`HTTP ${status}: ${text}`);
    this.status = status;
    this.body   = body;
    this.code   = code || undefined;
  }
}

let _baseOrigin = '';
let _baseToken  = '';

export function setBase({ port, token }) {
  if (port) _baseOrigin = `${location.protocol}//127.0.0.1:${port}`;
  if (token != null) _baseToken = token;
}

function baseOrigin(base) {
  if (!base) return _baseOrigin;
  if (base.origin) return base.origin;
  if (base.port) return `${location.protocol}//127.0.0.1:${base.port}`;
  return _baseOrigin;
}

function baseToken(base) {
  if (base && base.token != null) return base.token;
  return _baseToken || getToken() || '';
}

function fullUrl(path, base) {
  const origin = baseOrigin(base);
  return origin ? origin + path : path;
}

function sessionsPath(path, opts = {}) {
  return opts && opts.archived ? `${path}?archived=1` : path;
}

function usagePath(opts = {}) {
  const qs = new URLSearchParams();
  if (opts.days != null) qs.set('days', String(opts.days));
  if (opts.workspace) qs.set('workspace', String(opts.workspace));
  if (opts.timezoneOffsetMinutes != null) {
    qs.set('timezone_offset_minutes', String(opts.timezoneOffsetMinutes));
  }
  const text = qs.toString();
  return text ? `/api/usage?${text}` : '/api/usage';
}

export function sessionDraftPath(id, workspaceHash = '') {
  const sid = encodeURIComponent(id);
  const hash = String(workspaceHash || '').trim();
  if (hash) {
    return `/api/workspaces/${encodeURIComponent(hash)}/sessions/${sid}/draft`;
  }
  return `/api/sessions/${sid}/draft`;
}

export function sessionTodosPath(id, workspaceHash = '') {
  const sid = encodeURIComponent(id);
  const hash = String(workspaceHash || '').trim();
  if (hash) {
    return `/api/workspaces/${encodeURIComponent(hash)}/sessions/${sid}/todos`;
  }
  return `/api/sessions/${sid}/todos`;
}

export function sessionTitlePath(id, workspaceHash = '') {
  const sid = encodeURIComponent(id);
  const hash = String(workspaceHash || '').trim();
  if (hash) {
    return `/api/workspaces/${encodeURIComponent(hash)}/sessions/${sid}/title`;
  }
  return `/api/sessions/${sid}/title`;
}

async function request(method, path, body, base) {
  const headers = {};
  const token = baseToken(base);
  if (token) headers['X-ACECode-Token'] = token;
  if (body !== undefined) headers['Content-Type'] = 'application/json';

  const resp = await fetch(fullUrl(path, base), {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const ctype = resp.headers.get('Content-Type') || '';
  let parsed = null;
  if (resp.status !== 204 && ctype.includes('application/json')) {
    parsed = await resp.json().catch(() => null);
  } else if (resp.status !== 204) {
    parsed = await resp.text().catch(() => '');
  }
  if (!resp.ok) throw new ApiError(resp.status, parsed);
  return parsed;
}

export function createApi(base = null) {
  return {
    health:           ()             => request('GET',    '/api/health', undefined, base),
    // PUB 模型池负载快照(每 30s 轮询展示负载)。非敏感、无需 token。
    modelPoolStatus:  ()             => request('GET',    '/api/model-pool-status', undefined, base),
    // 控制台 PTY(add-console-dock):loopback-only,daemon 端 16 会话上限(429)。
    createPty:        (opts={})      => request('POST',   '/api/pty', opts, base),
    listPty:          ()             => request('GET',    '/api/pty', undefined, base),
    deletePty:        (id)           => request('DELETE', `/api/pty/${encodeURIComponent(id)}`, undefined, base),
    resizePty:        (id, cols, rows) =>
      request('POST', `/api/pty/${encodeURIComponent(id)}/resize`, { cols, rows }, base),
    setPtyTitle:      (id, title) =>
      request('POST', `/api/pty/${encodeURIComponent(id)}/title`, { title }, base),
    getUsageStats:    (opts={})      => request('GET',    usagePath(opts), undefined, base),
    listWorkspaces:   ()             => request('GET',    '/api/workspaces', undefined, base),
    registerWorkspace:(cwd)          => request('POST',   '/api/workspaces', {cwd}, base),
    pickWorkspaceFolder:()           => request('POST',   '/api/workspaces/pick-folder', undefined, base),
    // webapp 兼容模式(无 webview bridge)的「在资源管理器中打开」回退通路。
    // 仅 desktop 壳启动的 daemon 注册该端点(native_folder_picker_enabled 同款门控)。
    openInExplorer:   (path)         => request('POST',   '/api/open-in-explorer', { path }, base),
    listSessions:     (opts={})      => request('GET',    sessionsPath('/api/sessions', opts), undefined, base),
    createSession:    (opts={})      => request('POST',   '/api/sessions', opts, base),
    resumeSession:    (id)           => request('POST',   `/api/sessions/${encodeURIComponent(id)}/resume`, {}, base),
    listWorkspaceSessions:  (hash, opts={}) => request('GET',  sessionsPath(`/api/workspaces/${encodeURIComponent(hash)}/sessions`, opts), undefined, base),
    createWorkspaceSession: (hash, opts={}) => request('POST', `/api/workspaces/${encodeURIComponent(hash)}/sessions`, opts, base),
    resumeWorkspaceSession: (hash, id)    => request('POST', `/api/workspaces/${encodeURIComponent(hash)}/sessions/${encodeURIComponent(id)}/resume`, {}, base),
    archiveSession:   (id)           => request('PUT',    `/api/sessions/${encodeURIComponent(id)}/archive`, {}, base),
    unarchiveSession: (id)           => request('DELETE', `/api/sessions/${encodeURIComponent(id)}/archive`, undefined, base),
    archiveWorkspaceSession: (hash, id) =>
      request('PUT', `/api/workspaces/${encodeURIComponent(hash)}/sessions/${encodeURIComponent(id)}/archive`, {}, base),
    unarchiveWorkspaceSession: (hash, id) =>
      request('DELETE', `/api/workspaces/${encodeURIComponent(hash)}/sessions/${encodeURIComponent(id)}/archive`, undefined, base),
    getPinnedSessions: (hash) =>
      request('GET', `/api/workspaces/${encodeURIComponent(hash)}/pinned-sessions`, undefined, base),
    setPinnedSessions: (hash, sessionIds=[]) =>
      request('PUT', `/api/workspaces/${encodeURIComponent(hash)}/pinned-sessions`, { session_ids: sessionIds }, base),
    destroySession:   (id)           => request('DELETE', `/api/sessions/${encodeURIComponent(id)}`, undefined, base),
    getSessionDraft:  (id, workspaceHash = '') =>
      request('GET', sessionDraftPath(id, workspaceHash), undefined, base),
    setSessionDraft:  (id, text = '', workspaceHash = '') =>
      request('PUT', sessionDraftPath(id, workspaceHash), { text }, base),
    setSessionTitle:  (id, title = '', workspaceHash = '') =>
      request('PUT', sessionTitlePath(id, workspaceHash), { title }, base),
    clearSessionTodos: (id, workspaceHash = '') =>
      request('DELETE', sessionTodosPath(id, workspaceHash), undefined, base),
    sendInput:        (id, payload)  => {
      const body = payload && typeof payload === 'object' && !Array.isArray(payload)
        ? payload
        : { text: payload };
      return request('POST', `/api/sessions/${encodeURIComponent(id)}/messages`, body, base);
    },
    uploadSessionAttachment: (id, attachment) =>
      request('POST', `/api/sessions/${encodeURIComponent(id)}/attachments`, attachment, base),
    executeCommand:   (id, command)  => request('POST',   `/api/sessions/${encodeURIComponent(id)}/commands`, command, base),
    getMessages:      (id, since=0)  => request('GET',    `/api/sessions/${encodeURIComponent(id)}/messages?since=${since}`, undefined, base),
    listSkills:       ()             => request('GET',    '/api/skills', undefined, base),
    getSkillRoot:     (workspaceHash = '') => request('GET',
      '/api/skills/root' + (workspaceHash ? '?workspace=' + encodeURIComponent(workspaceHash) : ''),
      undefined, base),
    listCommands:     (workspaceHash) => request('GET',
      '/api/commands' + (workspaceHash ? '?workspace=' + encodeURIComponent(workspaceHash) : ''),
      undefined, base),
    setSkillEnabled:  (name, en)     => request('PUT',    `/api/skills/${encodeURIComponent(name)}`, {enabled: en}, base),
    getSkillBody:     (name)         => request('GET',    `/api/skills/${encodeURIComponent(name)}/body`, undefined, base),
    getMcp:           ()             => request('GET',    '/api/mcp', undefined, base),
    putMcp:           (cfg)          => request('PUT',    '/api/mcp', cfg, base),
    reloadMcp:        ()             => request('POST',   '/api/mcp/reload', undefined, base),
    listModels:       ()             => request('GET',    '/api/models', undefined, base),
    probeModels:      (draft)        => request('POST',   '/api/models/probe', draft, base),
    getSessionModel:  (sid, workspaceHash = '') => {
      const qs = workspaceHash ? `?workspace=${encodeURIComponent(workspaceHash)}` : '';
      return request('GET', `/api/sessions/${encodeURIComponent(sid)}/model${qs}`, undefined, base);
    },
    switchModel:      (sid, name)    => request('POST',   `/api/sessions/${encodeURIComponent(sid)}/model`, {name}, base),
    getSessionPermissionMode: (sid)  => request('GET',    `/api/sessions/${encodeURIComponent(sid)}/permissions`, undefined, base),
    setSessionPermissionMode: (sid, mode) => request('PUT', `/api/sessions/${encodeURIComponent(sid)}/permissions`, {mode}, base),
    getDefaultPermissionMode: ()     => request('GET',    '/api/config/default-permission-mode', undefined, base),
    setDefaultPermissionMode: (mode) => request('PUT',    '/api/config/default-permission-mode', {mode}, base),
    addModel:         (draft)        => request('POST',   '/api/models', draft, base),
    updateModel:      (name, draft)  => request('PUT',    `/api/models/${encodeURIComponent(name)}`, draft, base),
    removeModel:      (name)         => request('DELETE', `/api/models/${encodeURIComponent(name)}`, undefined, base),
    setDefaultModel:  (name)         => request('POST',   '/api/config/default-model', {name}, base),
    getDefaultModel:  ()             => request('GET',    '/api/config/default-model', undefined, base),
    getCopilotAuth:   ()             => request('GET',    '/api/copilot/auth', undefined, base),
    startCopilotAuth: ()             => request('POST',   '/api/copilot/auth/device', {}, base),
    pollCopilotAuth:  (deviceCode)   => request('POST',   '/api/copilot/auth/device/poll', { device_code: deviceCode }, base),
    logoutCopilot:    ()             => request('DELETE', '/api/copilot/auth', undefined, base),
    getUiPreferences: ()             => request('GET',    '/api/config/ui-preferences', undefined, base),
    setUiPreferences: (prefs)        => request('PUT',    '/api/config/ui-preferences', prefs, base),
    getUpgradeConfig: ()             => request('GET',    '/api/config/upgrade', undefined, base),
    setUpgradeConfig: (cfg)          => request('PUT',    '/api/config/upgrade', cfg, base),
    getUpdateStatus: ()              => request('GET',    '/api/update/status', undefined, base),
    startUpdate: ()                  => request('POST',   '/api/update/start', undefined, base),
    getAceBrowserBridge: ()          => request('GET',    '/api/config/ace-browser-bridge', undefined, base),
    setAceBrowserBridge: (cfg)       => request('PUT',    '/api/config/ace-browser-bridge', cfg, base),
    getHistory:       (cwd, max=100) => request('GET',    `/api/history?cwd=${encodeURIComponent(cwd)}&max=${max}`, undefined, base),
    appendHistory:    (text)         => request('POST',   '/api/history', {text}, base),
    forkSession:      (sid, atMessageId, title) =>
      request('POST', `/api/sessions/${encodeURIComponent(sid)}/fork`,
              { at_message_id: atMessageId, title: title || '' }, base),
    restoreSessionCheckpoint: (sid, atMessageId) =>
      request('POST',
        `/api/sessions/${encodeURIComponent(sid)}/file-checkpoints/${encodeURIComponent(atMessageId)}/restore`,
        {},
        base),

    // 跨 workspace 一次拿全 session 列表(SearchPalette 用)。
    // 返回 { sessions: [...], errors: [{hash, name, message}] }。
    // 单个 workspace 拉取失败不阻塞其它 workspace。
    listAllWorkspaceSessions: () => mergeAllWorkspaceSessions({
      listWorkspaces: () => request('GET', '/api/workspaces', undefined, base),
      listSessions: (hash) => request('GET',
        `/api/workspaces/${encodeURIComponent(hash)}/sessions`, undefined, base),
    }),
    listAllArchivedSessions: () => mergeAllWorkspaceSessions({
      listWorkspaces: () => request('GET', '/api/workspaces', undefined, base),
      listSessions: (hash) => request('GET',
        `/api/workspaces/${encodeURIComponent(hash)}/sessions?archived=1`, undefined, base),
    }),

    // SidePanel "文件" tab — 列指定目录的直接子项(不递归)。
    // path='' 列 cwd 根本身。showHidden=true 透出 dot 文件,但 noise 黑名单
    // (.git/node_modules/dist/build/__pycache__/.venv/venv/target/.next/.cache)
    // 始终过滤,不受 showHidden 影响。
    listFiles: (cwd, path, showHidden = false) => {
      const qs = `?cwd=${encodeURIComponent(cwd)}&path=${encodeURIComponent(path || '')}`
                 + (showHidden ? '&show_hidden=1' : '');
      return request('GET', '/api/files' + qs, undefined, base);
    },

    // SidePanel "预览" tab — 读单文件原文。返回 string(text body),不是 JSON,
    // 所以走自己的 fetch + r.text() 而非通用 request()。失败抛 ApiError(同 request 风格)。
    // 415 body 形如 {error:"file too large", size:N},供前端提示用户。
    readFile: async (cwd, path) => {
      const qs = `?cwd=${encodeURIComponent(cwd)}&path=${encodeURIComponent(path)}`;
      const headers = {};
      const token = baseToken(base);
      if (token) headers['X-ACECode-Token'] = token;
      const resp = await fetch(fullUrl('/api/files/content' + qs, base), {
        method: 'GET',
        headers,
      });
      if (!resp.ok) {
        let parsed = null;
        const ctype = resp.headers.get('Content-Type') || '';
        if (ctype.includes('application/json')) {
          parsed = await resp.json().catch(() => null);
        } else {
          parsed = await resp.text().catch(() => '');
        }
        throw new ApiError(resp.status, parsed);
      }
      return resp.text();
    },

    // SidePanel browser-native previews use an authenticated binary fetch. A
    // plain <img>/<object data="/api/..."> cannot attach the daemon token header.
    readFileBlob: async (cwd, path) => {
      const qs = `?cwd=${encodeURIComponent(cwd)}&path=${encodeURIComponent(path)}`;
      const headers = {};
      const token = baseToken(base);
      if (token) headers['X-ACECode-Token'] = token;
      const resp = await fetch(fullUrl('/api/files/blob' + qs, base), {
        method: 'GET',
        headers,
      });
      if (!resp.ok) {
        let parsed = null;
        const ctype = resp.headers.get('Content-Type') || '';
        if (ctype.includes('application/json')) {
          parsed = await resp.json().catch(() => null);
        } else {
          parsed = await resp.text().catch(() => '');
        }
        throw new ApiError(resp.status, parsed);
      }
      return resp.blob();
    },
  };
}

export const api = createApi();

// 抽出便于单测:对每个 workspace 并行 listSessions,失败收集到 errors[],成功扁平化
// 到 sessions[] 并注入 workspace_hash + workspaceName + cwd。
export async function mergeAllWorkspaceSessions({ listWorkspaces, listSessions }) {
  let workspaces = [];
  try {
    const list = await listWorkspaces();
    workspaces = Array.isArray(list) ? list : [];
  } catch (e) {
    return { sessions: [], errors: [{ hash: '', name: '', message: (e && e.message) || String(e) }] };
  }
  const settled = await Promise.allSettled(workspaces.map(async (w) => {
    const list = await listSessions(w.hash);
    return { ws: w, list: Array.isArray(list) ? list : [] };
  }));
  const sessions = [];
  const errors = [];
  for (let i = 0; i < settled.length; ++i) {
    const r = settled[i];
    const w = workspaces[i];
    if (r.status === 'fulfilled') {
      for (const s of r.value.list) {
        sessions.push({
          ...s,
          workspace_hash: s.workspace_hash || w.hash,
          workspaceName: w.name || w.cwd || w.hash,
          cwd: s.cwd || w.cwd,
        });
      }
    } else {
      errors.push({
        hash: w.hash,
        name: w.name || w.cwd || w.hash,
        message: (r.reason && r.reason.message) || String(r.reason || ''),
      });
    }
  }
  return { sessions, errors };
}
