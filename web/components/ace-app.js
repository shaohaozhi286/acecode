// 顶层容器:鉴权 gate(token-prompt 或 主界面)。

class AceApp extends HTMLElement {
  connectedCallback() {
    this.innerHTML = `<div class="ace-shell-host"></div>`;
    this.host = this.querySelector('.ace-shell-host');
    this.renderTokenPrompt();
    window.AceBus.addEventListener('health', () => this.renderShell());
    window.AceBus.addEventListener('need-token', () => this.renderTokenPrompt());
  }
  renderTokenPrompt() {
    this.host.innerHTML = `<ace-token-prompt></ace-token-prompt>`;
  }
  renderShell() {
    this.host.innerHTML = `
      <div class="ace-shell">
        <ace-sidebar></ace-sidebar>
        <ace-chat class="ace-main"></ace-chat>
      </div>
      <ace-permission-modal></ace-permission-modal>
      <ace-question-modal></ace-question-modal>
    `;
  }
}
customElements.define('ace-app', AceApp);
