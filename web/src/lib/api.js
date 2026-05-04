// HTTP 客户端封装:自动带 X-ACECode-Token,JSON 编解码,错误抛 ApiError。
//
// 共享 daemon 模型: workspace 是 API/session 数据,不是 daemon 端口归属。
// setBase({port, token}) 仍保留给 standalone/desktop bootstrap 与兼容场景。

import { getToken } from './auth.js';

export class ApiError extends Error {
  constructor(status, body) {
    super(`HTTP ${status}: ${typeof body === 'string' ? body : JSON.stringify(body)}`);
    this.status = status;
    this.body   = body;
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
    listWorkspaces:   ()             => request('GET',    '/api/workspaces', undefined, base),
    registerWorkspace:(cwd)          => request('POST',   '/api/workspaces', {cwd}, base),
    listSessions:     ()             => request('GET',    '/api/sessions', undefined, base),
    createSession:    (opts={})      => request('POST',   '/api/sessions', opts, base),
    resumeSession:    (id)           => request('POST',   `/api/sessions/${encodeURIComponent(id)}/resume`, {}, base),
    listWorkspaceSessions:  (hash)        => request('GET',  `/api/workspaces/${encodeURIComponent(hash)}/sessions`, undefined, base),
    createWorkspaceSession: (hash, opts={}) => request('POST', `/api/workspaces/${encodeURIComponent(hash)}/sessions`, opts, base),
    resumeWorkspaceSession: (hash, id)    => request('POST', `/api/workspaces/${encodeURIComponent(hash)}/sessions/${encodeURIComponent(id)}/resume`, {}, base),
    destroySession:   (id)           => request('DELETE', `/api/sessions/${encodeURIComponent(id)}`, undefined, base),
    sendInput:        (id, text)     => request('POST',   `/api/sessions/${encodeURIComponent(id)}/messages`, {text}, base),
    getMessages:      (id, since=0)  => request('GET',    `/api/sessions/${encodeURIComponent(id)}/messages?since=${since}`, undefined, base),
    listSkills:       ()             => request('GET',    '/api/skills', undefined, base),
    setSkillEnabled:  (name, en)     => request('PUT',    `/api/skills/${encodeURIComponent(name)}`, {enabled: en}, base),
    getSkillBody:     (name)         => request('GET',    `/api/skills/${encodeURIComponent(name)}/body`, undefined, base),
    getMcp:           ()             => request('GET',    '/api/mcp', undefined, base),
    putMcp:           (cfg)          => request('PUT',    '/api/mcp', cfg, base),
    reloadMcp:        ()             => request('POST',   '/api/mcp/reload', undefined, base),
    listModels:       ()             => request('GET',    '/api/models', undefined, base),
    switchModel:      (sid, name)    => request('POST',   `/api/sessions/${encodeURIComponent(sid)}/model`, {name}, base),
    getHistory:       (cwd, max=100) => request('GET',    `/api/history?cwd=${encodeURIComponent(cwd)}&max=${max}`, undefined, base),
    appendHistory:    (text)         => request('POST',   '/api/history', {text}, base),
    forkSession:      (sid, atMessageId, title) =>
      request('POST', `/api/sessions/${encodeURIComponent(sid)}/fork`,
              { at_message_id: atMessageId, title: title || '' }, base),

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
  };
}

export const api = createApi();
