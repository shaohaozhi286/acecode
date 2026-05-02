// WebSocket 连接 + 指数退避重连 + sessionStorage 持久化 last_seq。
// 服务端→客户端事件 dispatch 给 listener。客户端→服务端命令 send_*() 直接写 socket。

import { getToken } from './auth.js';

const MAX_BACKOFF_MS = 30_000;
const NORMAL_CLOSE = 1000;

function closeWs(ws, reason = 'client closing') {
  if (!ws) return;
  try { ws.close(NORMAL_CLOSE, reason); } catch {}
}

export class AceConnection extends EventTarget {
  constructor() {
    super();
    this.ws = null;
    this.sessionId = '';
    this.attempts = 0;
    this.lastSeq = 0;
    this.closing = false;
    this._host = '';   // 留空 → 用 location.host(单 workspace 默认)
    this._token = '';
  }

  reconfigure({ port, token }) {
    if (port !== undefined) this._host = port ? `127.0.0.1:${port}` : '';
    if (token != null) this._token = token;
    if (this.sessionId) this._open();
  }

  bind(sessionId) {
    this.unbind();
    this.sessionId = sessionId;
    this.lastSeq = parseInt(sessionStorage.getItem(`ace-seq-${sessionId}`) || '0', 10) || 0;
    this._open();
  }

  unbind() {
    if (this.ws) {
      this.closing = true;
      closeWs(this.ws, 'client unbind');
      this.ws = null;
    }
    this.sessionId = '';
    this.attempts = 0;
    this.closing = false;
  }

  _activeHost()  { return this._host  || location.host; }
  _activeToken() { return this._token || getToken() || ''; }

  _open() {
    if (!this.sessionId) return;
    if (this.ws) {
      try { this.ws.onopen = this.ws.onmessage = this.ws.onclose = this.ws.onerror = null; } catch {}
      closeWs(this.ws, 'client rebind');
    }
    const proto   = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const token   = this._activeToken();
    const tokenQs = token ? `?token=${encodeURIComponent(token)}` : '';
    const host    = this._activeHost();
    const url     = `${proto}//${host}/ws/sessions/${encodeURIComponent(this.sessionId)}${tokenQs}`;
    const ws = new WebSocket(url);
    this.ws = ws;
    ws.onopen = () => {
      if (this.ws !== ws) return;
      this.attempts = 0;
      this.dispatchEvent(new Event('open'));
      try {
        ws.send(JSON.stringify({
          type: 'hello',
          payload: { session_id: this.sessionId, since: this.lastSeq },
        }));
      } catch {}
    };
    ws.onmessage = (e) => {
      if (this.ws !== ws) return;
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      if (typeof msg.seq === 'number' && msg.seq > this.lastSeq) {
        this.lastSeq = msg.seq;
        sessionStorage.setItem(`ace-seq-${this.sessionId}`, String(this.lastSeq));
      }
      this.dispatchEvent(new CustomEvent('message', { detail: msg }));
    };
    const reconnect = () => {
      if (this.closing || !this.sessionId) return;
      if (this.ws !== ws) return;
      this.ws = null;
      const delay = Math.min(MAX_BACKOFF_MS, 1000 * Math.pow(2, this.attempts++));
      this.dispatchEvent(new CustomEvent('disconnect', { detail: { delay } }));
      setTimeout(() => this._open(), delay);
    };
    ws.onclose = reconnect;
    ws.onerror = () => closeWs(ws, 'client error');
  }

  sendUserInput(text)              { this._send({ type: 'user_input', payload: { text } }); }
  sendDecision(request_id, choice) { this._send({ type: 'decision',   payload: { request_id, choice } }); }
  sendQuestionAnswer(payload)      { this._send({ type: 'question_answer', payload }); }
  sendAbort()                      { this._send({ type: 'abort' }); }
  ping()                           { this._send({ type: 'ping' }); }

  _send(msg) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(JSON.stringify(msg));
  }
}

export const connection = new AceConnection();
