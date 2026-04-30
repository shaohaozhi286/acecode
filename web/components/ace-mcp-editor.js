// MCP JSON 编辑器:textarea + 客户端 JSON 校验 + 保存 + reload(v1 daemon 501)

import { api } from '../api.js';

class AceMcpEditor extends HTMLElement {
  async connectedCallback() {
    this.innerHTML = `
      <div>
        <textarea class="form-control font-monospace" rows="14" id="ace-mcp-text"></textarea>
        <div class="text-danger small mt-1" id="ace-mcp-err" hidden></div>
        <div class="d-flex gap-2 mt-2">
          <button class="btn btn-sm btn-primary"   id="ace-mcp-save"   disabled>保存</button>
          <button class="btn btn-sm btn-secondary" id="ace-mcp-reload">Reload</button>
        </div>
      </div>
    `;
    const ta  = this.querySelector('#ace-mcp-text');
    const err = this.querySelector('#ace-mcp-err');
    const save= this.querySelector('#ace-mcp-save');
    const rel = this.querySelector('#ace-mcp-reload');

    try {
      const cfg = await api.getMcp();
      ta.value = JSON.stringify(cfg, null, 2);
      save.disabled = false;
    } catch (e) { console.error(e); }

    ta.oninput = () => {
      try { JSON.parse(ta.value); err.hidden = true; save.disabled = false; }
      catch (e) { err.textContent = e.message; err.hidden = false; save.disabled = true; }
    };
    save.onclick = async () => {
      try {
        const obj = JSON.parse(ta.value);
        await api.putMcp(obj);
        alert('已保存。点 Reload 让 daemon 重连(v1 当前版本会提示需重启 daemon)。');
      } catch (e) { alert('保存失败: ' + e.message); }
    };
    rel.onclick = async () => {
      try { const r = await api.reloadMcp(); alert('Reload 结果: ' + JSON.stringify(r)); }
      catch (e) { alert(e.message + '\n(v1 daemon 不支持热重载,需要重启 daemon)'); }
    };
  }
}
customElements.define('ace-mcp-editor', AceMcpEditor);
