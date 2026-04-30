// Skills 面板:列出所有 skill + 启停开关 + 查看正文。

import { api } from '../api.js';

class AceSkillsPanel extends HTMLElement {
  async connectedCallback() {
    this.innerHTML = `<div class="ace-skills-list"></div>`;
    this.list = this.querySelector('.ace-skills-list');
    await this.refresh();
  }

  async refresh() {
    try {
      const skills = await api.listSkills();
      this.list.innerHTML = '';
      skills.forEach(s => {
        const row = document.createElement('div');
        row.className = 'd-flex align-items-center justify-content-between border-bottom py-1 px-1 small';
        row.innerHTML = `
          <div style="flex:1;min-width:0">
            <div class="text-truncate"><strong>${escapeHtml(s.name)}</strong></div>
            <div class="text-muted text-truncate" style="font-size:0.75em">${escapeHtml(s.description || '')}</div>
          </div>
          <div>
            <input type="checkbox" class="form-check-input me-2" ${s.enabled ? 'checked' : ''}>
            <button class="btn btn-link btn-sm p-0">查看</button>
          </div>
        `;
        const toggle = row.querySelector('input');
        toggle.onchange = async () => {
          try { await api.setSkillEnabled(s.name, toggle.checked); }
          catch (e) { toggle.checked = !toggle.checked; alert('切换失败: ' + e.message); }
        };
        row.querySelector('button').onclick = async () => {
          try {
            const body = await api.getSkillBody(s.name);
            const w = window.open('', '_blank', 'width=600,height=600');
            w.document.body.innerText = body;
          } catch (e) { alert('查看失败: ' + e.message); }
        };
        this.list.appendChild(row);
      });
    } catch (e) { console.error(e); }
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

customElements.define('ace-skills-panel', AceSkillsPanel);
