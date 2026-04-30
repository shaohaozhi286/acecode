// permission_request 弹框。Allow / Deny / AllowSession 三按钮。
// 多 request 排队;ESC 等价 Deny。

import { connection } from '../connection.js';

class AcePermissionModal extends HTMLElement {
  constructor() {
    super();
    this.queue = [];
    this.current = null;
  }

  connectedCallback() {
    this.innerHTML = `
      <div class="modal" tabindex="-1" id="ace-perm-modal" data-bs-backdrop="static">
        <div class="modal-dialog">
          <div class="modal-content">
            <div class="modal-header"><h5 class="modal-title">需要授权工具调用</h5></div>
            <div class="modal-body">
              <div><strong>工具:</strong> <span class="ace-perm-tool"></span></div>
              <pre class="ace-perm-args mt-2 p-2 bg-light"></pre>
            </div>
            <div class="modal-footer">
              <button type="button" class="btn btn-success" data-choice="allow">允许</button>
              <button type="button" class="btn btn-warning" data-choice="allow_session">本次会话允许</button>
              <button type="button" class="btn btn-danger"  data-choice="deny">拒绝</button>
            </div>
          </div>
        </div>
      </div>
    `;
    this.modalEl = this.querySelector('#ace-perm-modal');
    this.bsModal = window.bootstrap?.Modal ? new window.bootstrap.Modal(this.modalEl) : null;
    this.querySelectorAll('button[data-choice]').forEach(b => {
      b.onclick = () => this.respond(b.dataset.choice);
    });
    this.modalEl.addEventListener('hidden.bs.modal', () => {
      // ESC / 关闭 = deny
      if (this.current) this.respond('deny');
    });

    window.AceBus.addEventListener('permission-request', e => this.enqueue(e.detail));
  }

  enqueue(payload) {
    this.queue.push(payload);
    if (!this.current) this.next();
  }

  next() {
    this.current = this.queue.shift();
    if (!this.current) return;
    this.querySelector('.ace-perm-tool').textContent = this.current.tool || '';
    this.querySelector('.ace-perm-args').textContent =
      JSON.stringify(this.current.args || {}, null, 2);
    if (this.bsModal) this.bsModal.show();
  }

  respond(choice) {
    if (!this.current) return;
    connection.sendDecision(this.current.request_id, choice);
    const tmp = this.current;
    this.current = null;
    if (this.bsModal) this.bsModal.hide();
    setTimeout(() => this.next(), 100);
    void tmp;
  }
}
customElements.define('ace-permission-modal', AcePermissionModal);
