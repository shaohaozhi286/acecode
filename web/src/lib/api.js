// HTTP 客户端封装:自动带 X-ACECode-Token,JSON 编解码,错误抛 ApiError。
//
// 多 workspace 模型: setBase({port, token}) 切到不同 daemon;不调时用 location.host
// 与 getToken()(浏览器单 workspace 路径完全不变)。

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
    listSessions:     ()             => request('GET',    '/api/sessions', undefined, base),
    createSession:    (opts={})      => request('POST',   '/api/sessions', opts, base),
    resumeSession:    (id)           => request('POST',   `/api/sessions/${encodeURIComponent(id)}/resume`, {}, base),
    destroySession:   (id)           => request('DELETE', `/api/sessions/${encodeURIComponent(id)}`, undefined, base),
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
  };
}

export const api = createApi();
