// HTTP 客户端封装:自动带 X-ACECode-Token,JSON 编解码,错误抛 AceApiError。

import { getToken } from './auth.js';

export class AceApiError extends Error {
  constructor(status, body) {
    super(`HTTP ${status}: ${typeof body === 'string' ? body : JSON.stringify(body)}`);
    this.status = status;
    this.body   = body;
  }
}

async function request(method, path, body) {
  const headers = {};
  const token = getToken();
  if (token) headers['X-ACECode-Token'] = token;
  if (body !== undefined) headers['Content-Type'] = 'application/json';

  const resp = await fetch(path, {
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
  if (!resp.ok) throw new AceApiError(resp.status, parsed);
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
