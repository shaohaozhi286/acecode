// AskUserQuestion modal。1-4 questions × 2-4 options + 自动追加 "Other..."。
// multiSelect=false → radio,multiSelect=true → checkbox。
// ESC = cancelled,daemon 视为 user cancelled。

import { connection } from '../connection.js';

class AceQuestionModal extends HTMLElement {
  constructor() {
    super();
    this.current = null;
  }

  connectedCallback() {
    this.innerHTML = `
      <div class="modal" tabindex="-1" id="ace-q-modal" data-bs-backdrop="static">
        <div class="modal-dialog modal-lg">
          <div class="modal-content">
            <div class="modal-header"><h5 class="modal-title">需要回答</h5></div>
            <div class="modal-body" id="ace-q-body" style="max-height:60vh;overflow-y:auto"></div>
            <div class="modal-footer">
              <button type="button" class="btn btn-secondary" data-act="cancel">取消</button>
              <button type="button" class="btn btn-primary"   data-act="submit" disabled>提交</button>
            </div>
          </div>
        </div>
      </div>
    `;
    this.modalEl  = this.querySelector('#ace-q-modal');
    this.bodyEl   = this.querySelector('#ace-q-body');
    this.submitBtn = this.querySelector('[data-act="submit"]');
    this.cancelBtn = this.querySelector('[data-act="cancel"]');
    this.bsModal  = window.bootstrap?.Modal ? new window.bootstrap.Modal(this.modalEl) : null;

    this.cancelBtn.onclick = () => this.respond(true);
    this.submitBtn.onclick = () => this.respond(false);
    this.modalEl.addEventListener('hidden.bs.modal', () => {
      if (this.current) this.respond(true);
    });

    window.AceBus.addEventListener('question-request', e => this.show(e.detail));
  }

  show(payload) {
    this.current = payload;
    this.bodyEl.innerHTML = '';
    (payload.questions || []).forEach((q, qi) => {
      const block = document.createElement('div');
      block.className = 'mb-3';
      block.innerHTML = `<div class="mb-2 fw-semibold">${escapeHtml(q.text || q.question)}</div>`;
      const inputType = q.multiSelect ? 'checkbox' : 'radio';
      (q.options || []).forEach((opt, oi) => {
        const id = `q-${qi}-${oi}`;
        const wrap = document.createElement('div');
        wrap.className = 'form-check';
        wrap.innerHTML = `
          <input class="form-check-input ace-q-opt" type="${inputType}" name="ace-q-${qi}" id="${id}" value="${escapeHtml(opt.value || opt.label)}">
          <label class="form-check-label" for="${id}">${escapeHtml(opt.label)}</label>
        `;
        wrap.querySelector('input').onchange = () => this.refreshSubmitEnabled();
        block.appendChild(wrap);
      });
      // Other input
      const other = document.createElement('div');
      other.className = 'mt-2';
      other.innerHTML = `
        <input type="text" class="form-control form-control-sm ace-q-other" placeholder='Other... (自定义)' />
      `;
      other.querySelector('input').oninput = () => this.refreshSubmitEnabled();
      block.appendChild(other);
      block.dataset.qIndex = String(qi);
      block.dataset.qid = q.id || q.question || '';
      this.bodyEl.appendChild(block);
    });
    this.refreshSubmitEnabled();
    if (this.bsModal) this.bsModal.show();
  }

  refreshSubmitEnabled() {
    // 每个 question 至少有一个答案(选了 option 或填了 custom_text)
    const blocks = this.bodyEl.querySelectorAll('[data-q-index]');
    let ok = blocks.length > 0;
    blocks.forEach(b => {
      const checked = b.querySelectorAll('.ace-q-opt:checked').length > 0;
      const other   = b.querySelector('.ace-q-other').value.trim().length > 0;
      if (!checked && !other) ok = false;
    });
    this.submitBtn.disabled = !ok;
  }

  respond(cancelled) {
    if (!this.current) return;
    let payload;
    if (cancelled) {
      payload = { request_id: this.current.request_id, cancelled: true };
    } else {
      const answers = [];
      this.bodyEl.querySelectorAll('[data-q-index]').forEach(b => {
        const qid     = b.dataset.qid;
        const checked = Array.from(b.querySelectorAll('.ace-q-opt:checked')).map(i => i.value);
        const custom  = b.querySelector('.ace-q-other').value.trim();
        const ans = { question_id: qid, selected: checked };
        if (custom) ans.custom_text = custom;
        answers.push(ans);
      });
      payload = { request_id: this.current.request_id, answers };
    }
    connection.sendQuestionAnswer(payload);
    this.current = null;
    if (this.bsModal) this.bsModal.hide();
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

customElements.define('ace-question-modal', AceQuestionModal);
