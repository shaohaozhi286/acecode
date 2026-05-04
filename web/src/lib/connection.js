// WebSocket 连接 + 指数退避重连 + sessionStorage 持久化 last_seq。
// 服务端→客户端事件 dispatch 给 listener。客户端→服务端命令 send_*() 直接写 socket。

import { getToken } from './auth.js';

const MAX_BACKOFF_MS = 30_000;
const NORMAL_CLOSE = 1000;

function closeWs(ws, reason = 'client closing') {
  if (!ws) return;
  try { ws.close(NORMAL_CLOSE, reason); } catch {}
}

export function createSessionSubscriptionManager({ subscribe, unsubscribe } = {}) {
  const refs = new Map();
  return {
    retain(sessionId) {
      if (!sessionId) return 0;
      const current = refs.get(sessionId) || 0;
      const next = current + 1;
      refs.set(sessionId, next);
      if (current === 0) subscribe?.(sessionId);
      return next;
    },
    release(sessionId) {
      if (!sessionId) return 0;
      const current = refs.get(sessionId) || 0;
      if (current <= 1) {
        refs.delete(sessionId);
        if (current === 1) unsubscribe?.(sessionId);
        return 0;
      }
      const next = current - 1;
      refs.set(sessionId, next);
      return next;
    },
    count(sessionId) {
      return refs.get(sessionId) || 0;
    },
    snapshot() {
      return new Map(refs);
    },
    clear() {
      refs.clear();
    },
  };
}

export class AceConnection extends EventTarget {
  constructor() {
    super();
    this.ws = null;
    this.sessionId = '';
    this.sessions = new Map();
    this.statusWorkspaces = new Set();
    this.attempts = 0;
    this.closing = false;
    this._host = '';   // 留空 → 用 location.host(单 workspace 默认)
    this._token = '';
    this.sessionRefs = createSessionSubscriptionManager({
      subscribe: (sessionId) => this.subscribe(sessionId),
      unsubscribe: (sessionId) => this.unsubscribe(sessionId),
    });
  }

  reconfigure({ port, token }) {
    const nextHost = port !== undefined ? (port ? `127.0.0.1:${port}` : '') : this._host;
    const nextToken = token != null ? token : this._token;
    const changed = nextHost !== this._host || nextToken !== this._token;
    this._host = nextHost;
    this._token = nextToken;
    if (changed && (this.sessions.size || this.statusWorkspaces.size)) this._reopen();
  }

  bind(sessionId) {
    this.subscribe(sessionId);
  }

  retainSession(sessionId) {
    return this.sessionRefs.retain(sessionId);
  }

  releaseSession(sessionId) {
    return this.sessionRefs.release(sessionId);
  }

  subscribe(sessionId) {
    if (!sessionId) return;
    this.sessionId = sessionId;
    if (!this.sessions.has(sessionId)) {
      const lastSeq = parseInt(sessionStorage.getItem(`ace-seq-${sessionId}`) || '0', 10) || 0;
      this.sessions.set(sessionId, { lastSeq });
    }
    if (!this.ws || this.ws.readyState === WebSocket.CLOSED) this._open();
    else if (this.ws.readyState === WebSocket.OPEN) this._sendSubscribe(sessionId);
  }

  unsubscribe(sessionId) {
    if (!sessionId || !this.sessions.has(sessionId)) return;
    this._send({ type: 'unsubscribe', payload: { session_id: sessionId } });
    this.sessions.delete(sessionId);
    if (this.sessionId === sessionId) this.sessionId = this.sessions.keys().next().value || '';
    if (this.sessions.size === 0 && this.statusWorkspaces.size === 0) this._closeSocket('client no sessions');
  }

  subscribeWorkspaceStatus(workspaceHash) {
    if (!workspaceHash) return;
    this.statusWorkspaces.add(workspaceHash);
    if (!this.ws || this.ws.readyState === WebSocket.CLOSED) this._open();
    else if (this.ws.readyState === WebSocket.OPEN) this._sendStatusSubscribe(workspaceHash);
  }

  unsubscribeWorkspaceStatus(workspaceHash) {
    if (!workspaceHash || !this.statusWorkspaces.has(workspaceHash)) return;
    this._send({ type: 'status_unsubscribe', payload: { workspace_hash: workspaceHash } });
    this.statusWorkspaces.delete(workspaceHash);
    if (this.sessions.size === 0 && this.statusWorkspaces.size === 0) this._closeSocket('client no status scopes');
  }

  unbind() {
    this.sessions.clear();
    this.statusWorkspaces.clear();
    this.sessionRefs.clear();
    if (this.ws) {
      this._closeSocket('client unbind');
    }
    this.sessionId = '';
    this.attempts = 0;
    this.closing = false;
  }

  _activeHost()  { return this._host  || location.host; }
  _activeToken() { return this._token || getToken() || ''; }

  _closeSocket(reason) {
    if (!this.ws) return;
    this.closing = true;
    try { this.ws.onopen = this.ws.onmessage = this.ws.onclose = this.ws.onerror = null; } catch {}
    closeWs(this.ws, reason);
    this.ws = null;
    this.closing = false;
  }

  _reopen() {
    this._closeSocket('client reconfigure');
    this._open();
  }

  _sendSubscribe(sessionId) {
    const state = this.sessions.get(sessionId);
    if (!state) return;
    this._send({
      type: 'subscribe',
      payload: { session_id: sessionId, since: state.lastSeq || 0 },
    });
  }

  _sendStatusSubscribe(workspaceHash) {
    this._send({
      type: 'status_subscribe',
      payload: { workspace_hash: workspaceHash },
    });
  }

  _open() {
    if (this.sessions.size === 0 && this.statusWorkspaces.size === 0) return;
    if (this.ws) {
      try { this.ws.onopen = this.ws.onmessage = this.ws.onclose = this.ws.onerror = null; } catch {}
      closeWs(this.ws, 'client rebind');
    }
    const proto   = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const token   = this._activeToken();
    const tokenQs = token ? `?token=${encodeURIComponent(token)}` : '';
    const host    = this._activeHost();
    const url     = `${proto}//${host}/ws/sessions/_multiplex${tokenQs}`;
    const ws = new WebSocket(url);
    this.ws = ws;
    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.attempts = 0;
      this.dispatchEvent(new Event('open'));
      for (const sessionId of this.sessions.keys()) this._sendSubscribe(sessionId);
      for (const workspaceHash of this.statusWorkspaces.keys()) this._sendStatusSubscribe(workspaceHash);
    };
    ws.onmessage = (e) => {
      if (this.ws !== ws) return;
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      const sid = msg.session_id || msg.payload?.session_id || this.sessionId || '';
      if (sid) msg.session_id = sid;
      if (sid && typeof msg.seq === 'number') {
        const state = this.sessions.get(sid) || { lastSeq: 0 };
        if (msg.seq > state.lastSeq) {
          state.lastSeq = msg.seq;
          this.sessions.set(sid, state);
          sessionStorage.setItem(`ace-seq-${sid}`, String(state.lastSeq));
        }
      }
      this.dispatchEvent(new CustomEvent('message', { detail: msg }));
    };
    const reconnect = () => {
      if (this.closing || (this.sessions.size === 0 && this.statusWorkspaces.size === 0)) return;
      if (this.ws !== ws) return;
      this.ws = null;
      const delay = Math.min(MAX_BACKOFF_MS, 1000 * Math.pow(2, this.attempts++));
      this.dispatchEvent(new CustomEvent('disconnect', { detail: { delay } }));
      setTimeout(() => this._open(), delay);
    };
    ws.onclose = reconnect;
    ws.onerror = () => closeWs(ws, 'client error');
  }

  sendUserInput(text, sessionId=this.sessionId) { this._send({ type: 'user_input', payload: { session_id: sessionId, text } }); }
  sendDecision(request_id, choice, sessionId=this.sessionId) { this._send({ type: 'decision', payload: { session_id: sessionId, request_id, choice } }); }
  sendQuestionAnswer(payload)      { this._send({ type: 'question_answer', payload }); }
  sendAbort(sessionId=this.sessionId) { this._send({ type: 'abort', payload: { session_id: sessionId } }); }
  markSessionRead({ sessionId=this.sessionId, workspaceHash='', cursor=0 } = {}) {
    if (!sessionId) return;
    this._send({
      type: 'mark_session_read',
      payload: { session_id: sessionId, workspace_hash: workspaceHash || '', cursor: cursor || 0 },
    });
  }
  ping()                           { this._send({ type: 'ping' }); }

  _send(msg) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(JSON.stringify(msg));
  }
}

export const connection = new AceConnection();
