// 聊天主区:头部 + 消息列表 + 输入框。订阅 connection 事件并分发渲染。
// 输入历史 ↑/↓:per-cwd 拉一次,本地内存翻页。提交时同时 WS user_input + POST history。

import { api } from '../api.js';
import { connection } from '../connection.js';

class AceChat extends HTMLElement {
  constructor() {
    super();
    this.sessionId = '';
    this.cwd = '';
    this.history = [];
    this.histPtr = -1;
    this.toolBlocks = new Map();    // tool name → ace-tool-block element (most recent unfinished)
    this.currentAssistant = null;   // streaming assistant message
    this.busy = false;
  }

  async connectedCallback() {
    this.innerHTML = `
      <div class="d-flex flex-column h-100">
        <div class="d-flex align-items-center justify-content-between p-2 border-bottom">
          <div><strong class="ace-session-title">未选择会话</strong></div>
          <div class="d-flex align-items-center gap-2">
            <ace-model-picker></ace-model-picker>
            <button class="btn btn-sm btn-outline-danger" id="ace-abort-btn" disabled>Abort</button>
            <span id="ace-spinner" hidden><span class="ace-spinner"></span> 处理中…</span>
          </div>
        </div>
        <div class="ace-chat-messages flex-grow-1"></div>
        <div class="ace-chat-input">
          <textarea class="form-control" rows="2" id="ace-input" placeholder="输入消息,Enter 发送,Shift+Enter 换行"></textarea>
        </div>
        <div id="ace-toast-host" class="position-fixed bottom-0 end-0 p-3"></div>
      </div>
    `;
    this.titleEl   = this.querySelector('.ace-session-title');
    this.messages  = this.querySelector('.ace-chat-messages');
    this.input     = this.querySelector('#ace-input');
    this.abortBtn  = this.querySelector('#ace-abort-btn');
    this.spinner   = this.querySelector('#ace-spinner');
    this.modelPick = this.querySelector('ace-model-picker');
    this.toastHost = this.querySelector('#ace-toast-host');

    this.input.addEventListener('keydown', (e) => this.onKey(e));
    this.abortBtn.onclick = () => connection.sendAbort();

    connection.addEventListener('message', e => this.onWsMessage(e.detail));
    window.AceBus.addEventListener('session-select', e => this.selectSession(e.detail.id));
    window.AceBus.addEventListener('toast', e => this.toast(e.detail));

    // 拿 health.cwd 作为 input history scope
    try { const h = await api.health(); this.cwd = h.cwd || ''; await this.loadHistory(); }
    catch {}
  }

  async loadHistory() {
    if (!this.cwd) return;
    try { this.history = await api.getHistory(this.cwd, 200); }
    catch { this.history = []; }
  }

  async selectSession(id) {
    if (this.sessionId === id) return;
    this.sessionId = id;
    this.titleEl.textContent = id;
    this.messages.innerHTML = '';
    this.toolBlocks.clear();
    this.currentAssistant = null;
    this.modelPick.setAttribute('session-id', id);
    connection.bind(id);
    // 先拉历史
    try {
      const data = await api.getMessages(id, 0);
      const msgs = (data && data.messages) || [];
      msgs.forEach(m => this.appendMessage(m.role, m.content || ''));
      const events = (data && data.events) || [];
      events.forEach(ev => this.onWsMessage(ev));
    } catch (e) { console.error(e); }
  }

  onKey(e) {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      this.submit();
    } else if (e.key === 'ArrowUp' && this.atFirstLine()) {
      e.preventDefault();
      if (this.history.length === 0) return;
      if (this.histPtr === -1) this.histPtr = this.history.length;
      if (this.histPtr > 0) this.histPtr--;
      this.input.value = this.history[this.histPtr] || '';
    } else if (e.key === 'ArrowDown' && this.atLastLine()) {
      if (this.histPtr === -1) return;
      e.preventDefault();
      this.histPtr++;
      if (this.histPtr >= this.history.length) {
        this.histPtr = -1;
        this.input.value = '';
      } else {
        this.input.value = this.history[this.histPtr];
      }
    }
  }
  atFirstLine() {
    const v = this.input.value.substring(0, this.input.selectionStart);
    return !v.includes('\n');
  }
  atLastLine() {
    const v = this.input.value.substring(this.input.selectionEnd);
    return !v.includes('\n');
  }

  submit() {
    const text = this.input.value;
    if (!text.trim() || !this.sessionId) return;
    connection.sendUserInput(text);
    api.appendHistory(text).catch(e => console.warn('history append', e));
    this.history.push(text);
    this.histPtr = -1;
    this.input.value = '';
    this.appendMessage('user', text);
  }

  onWsMessage(msg) {
    const t = msg.type;
    const p = msg.payload || {};
    if (t === 'token') {
      if (!this.currentAssistant) {
        this.currentAssistant = this.appendMessage('assistant', '');
      }
      this.currentAssistant.appendDelta(p.text || '');
    } else if (t === 'message') {
      this.currentAssistant = null;
      this.appendMessage(p.role || 'system', p.content || '');
    } else if (t === 'busy_changed') {
      this.busy = !!p.busy;
      this.spinner.hidden = !this.busy;
      this.abortBtn.disabled = !this.busy;
    } else if (t === 'done' || t === 'error') {
      this.busy = false;
      this.spinner.hidden = true;
      this.abortBtn.disabled = true;
      this.currentAssistant = null;
      if (t === 'error') this.toast({ kind: 'err', text: '错误: ' + (p.reason || '') });
    } else if (t === 'tool_start') {
      const block = document.createElement('ace-tool-block');
      this.messages.appendChild(block);
      block.applyStart(p);
      this.toolBlocks.set(p.tool || '_anon', block);
      this.currentAssistant = null;
    } else if (t === 'tool_update') {
      const block = this.toolBlocks.get(p.tool || '_anon');
      if (block) block.applyUpdate(p);
    } else if (t === 'tool_end') {
      const block = this.toolBlocks.get(p.tool || '_anon');
      if (block) block.applyEnd(p);
      this.toolBlocks.delete(p.tool || '_anon');
    } else if (t === 'permission_request') {
      window.AceBus.dispatchEvent(new CustomEvent('permission-request', { detail: p }));
    } else if (t === 'question_request') {
      window.AceBus.dispatchEvent(new CustomEvent('question-request', { detail: p }));
    }
    // 自动滚到底
    this.messages.scrollTop = this.messages.scrollHeight;
  }

  appendMessage(role, content) {
    const m = document.createElement('ace-message');
    m.setAttribute('role', role);
    m.setAttribute('content', content);
    this.messages.appendChild(m);
    return m;
  }

  toast({ kind, text }) {
    const el = document.createElement('div');
    el.className = `toast align-items-center text-bg-${kind === 'ok' ? 'success' : 'danger'} border-0 show`;
    el.style.minWidth = '240px';
    el.innerHTML = `<div class="d-flex"><div class="toast-body">${text}</div></div>`;
    this.toastHost.appendChild(el);
    setTimeout(() => el.remove(), 3500);
  }
}
customElements.define('ace-chat', AceChat);
