// 远程访问时弹出的 token 输入。提交后调 setToken + 重探活。

class AceTokenPrompt extends HTMLElement {
  connectedCallback() {
    this.innerHTML = `
      <div class="container py-5" style="max-width: 480px">
        <h4 class="mb-3">需要访问令牌</h4>
        <p class="text-muted">从 <code>~/.acecode/run/token</code> 或运行
          <code>acecode daemon status</code> 查看 token,粘贴到下面。</p>
        <input type="password" class="form-control mb-2" id="ace-token-input" placeholder="X-ACECode-Token">
        <button class="btn btn-primary" id="ace-token-submit">提交</button>
        <div class="text-danger small mt-2" id="ace-token-err" hidden></div>
      </div>
    `;
    this.querySelector('#ace-token-submit').onclick = async () => {
      const v = this.querySelector('#ace-token-input').value.trim();
      const err = this.querySelector('#ace-token-err');
      err.hidden = true;
      if (!v) return;
      try {
        await window.AceTokenSubmit(v);
      } catch (e) {
        err.textContent = '认证失败: ' + e.message;
        err.hidden = false;
      }
    };
  }
}
customElements.define('ace-token-prompt', AceTokenPrompt);
