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

function fullUrl(path) {
  return _baseOrigin ? _baseOrigin + path : path;
}

async function request(method, path, body) {
  const headers = {};
  const token = _baseToken || getToken() || '';
  if (token) headers['X-ACECode-Token'] = token;
  if (body !== undefined) headers['Content-Type'] = 'application/json';

  const resp = await fetch(fullUrl(path), {
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

export const api = {
  health:           ()             => request('GET',    '/api/health'),
  listSessions:     ()             => request('GET',    '/api/sessions'),
  createSession:    (opts={})      => request('POST',   '/api/sessions', opts),
  destroySession:   (id)           => request('DELETE', `/api/sessions/${encodeURIComponent(id)}`),
  getMessages:      (id, since=0)  => request('GET',    `/api/sessions/${encodeURIComponent(id)}/messages?since=${since}`),
  listSkills:       ()             => request('GET',    '/api/skills'),
  setSkillEnabled:  (name, en)     => request('PUT',    `/api/skills/${encodeURIComponent(name)}`, {enabled: en}),
  getSkillBody:     (name)         => request('GET',    `/api/skills/${encodeURIComponent(name)}/body`),
  getMcp:           ()             => request('GET',    '/api/mcp'),
  putMcp:           (cfg)          => request('PUT',    '/api/mcp', cfg),
  reloadMcp:        ()             => request('POST',   '/api/mcp/reload'),
  listModels:       ()             => request('GET',    '/api/models'),
  switchModel:      (sid, name)    => request('POST',   `/api/sessions/${encodeURIComponent(sid)}/model`, {name}),
  getHistory:       (cwd, max=100) => request('GET',    `/api/history?cwd=${encodeURIComponent(cwd)}&max=${max}`),
  appendHistory:    (text)         => request('POST',   '/api/history', {text}),
};
