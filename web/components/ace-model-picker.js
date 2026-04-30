// 顶部模型下拉:GET /api/models 拉列表;切换 → POST /api/sessions/:id/model。
// 失败回滚 + 红色 toast。当前 session 切换 → 重新拉一次 effective model。

import { api } from '../api.js';

class AceModelPicker extends HTMLElement {
  static get observedAttributes() { return ['session-id']; }
  connectedCallback() {
    this.render();
    this.refresh();
  }
  attributeChangedCallback(name, _old, _new) {
    if (name === 'session-id') this.render();
  }

  async refresh() {
    try {
      const list = await api.listModels();
      const sel = this.querySelector('select');
      sel.innerHTML = '';
      list.forEach(m => {
        const opt = document.createElement('option');
        opt.value = m.name;
        opt.textContent = m.name + (m.is_legacy ? ' (legacy)' : '');
        sel.appendChild(opt);
      });
    } catch (e) { console.error(e); }
  }

  render() {
    this.innerHTML = `
      <div class="d-inline-flex align-items-center gap-2">
        <select class="form-select form-select-sm" style="width:auto"></select>
      </div>
    `;
    this.querySelector('select').onchange = async (e) => {
      const sid = this.getAttribute('session-id');
      const name = e.target.value;
      if (!sid) return;
      const orig = e.target.value;
      e.target.disabled = true;
      try {
        await api.switchModel(sid, name);
        window.AceBus.dispatchEvent(new CustomEvent('toast', {
          detail: { kind: 'ok', text: 'Switched to ' + name }
        }));
      } catch (err) {
        e.target.value = orig;
        window.AceBus.dispatchEvent(new CustomEvent('toast', {
          detail: { kind: 'err', text: '切换失败: ' + err.message }
        }));
      } finally {
        e.target.disabled = false;
      }
    };
  }
}
customElements.define('ace-model-picker', AceModelPicker);
