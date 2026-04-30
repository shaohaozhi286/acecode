// 左侧面板:Sessions / Skills / MCP tab 切换。session 列表为主 tab。

import { api } from '../api.js';

class AceSidebar extends HTMLElement {
  async connectedCallback() {
    this.innerHTML = `
      <div class="ace-sidebar p-2">
        <ul class="nav nav-pills nav-fill mb-2">
          <li class="nav-item"><a class="nav-link active" data-tab="sessions" href="#">会话</a></li>
          <li class="nav-item"><a class="nav-link" data-tab="skills" href="#">Skills</a></li>
          <li class="nav-item"><a class="nav-link" data-tab="mcp" href="#">MCP</a></li>
        </ul>
        <div data-pane="sessions">
          <button class="btn btn-sm btn-outline-primary mb-2 w-100" id="ace-new-session">+ 新建会话</button>
          <div id="ace-session-list" class="list-group"></div>
        </div>
        <div data-pane="skills" hidden><ace-skills-panel></ace-skills-panel></div>
        <div data-pane="mcp" hidden><ace-mcp-editor></ace-mcp-editor></div>
      </div>
    `;
    this.querySelectorAll('a[data-tab]').forEach(a => {
      a.onclick = (e) => {
        e.preventDefault();
        this.querySelectorAll('a[data-tab]').forEach(x => x.classList.remove('active'));
        a.classList.add('active');
        this.querySelectorAll('[data-pane]').forEach(p => p.hidden = p.dataset.pane !== a.dataset.tab);
      };
    });
    this.querySelector('#ace-new-session').onclick = async () => {
      try {
        const res = await api.createSession({});
        await this.refresh();
        this.dispatchSelect(res.session_id);
      } catch (e) { console.error(e); }
    };
    await this.refresh();
  }

  async refresh() {
    try {
      const list = await api.listSessions();
      const root = this.querySelector('#ace-session-list');
      root.innerHTML = '';
      list.forEach(s => {
        const a = document.createElement('a');
        a.href = '#';
        a.className = 'list-group-item list-group-item-action small';
        a.textContent = (s.title || s.id) + (s.active ? ' ●' : '');
        a.onclick = (e) => { e.preventDefault(); this.dispatchSelect(s.id); };
        root.appendChild(a);
      });
    } catch (e) { console.error(e); }
  }

  dispatchSelect(id) {
    window.AceBus.dispatchEvent(new CustomEvent('session-select', { detail: { id } }));
  }
}
customElements.define('ace-sidebar', AceSidebar);
