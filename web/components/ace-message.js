// 单条消息渲染。user / assistant / tool / system 区分样式。
// markdown 渲染 v1 简化:转义 HTML + 识别代码块 + 加粗/斜体/inline code/链接,其余原样。

class AceMessage extends HTMLElement {
  static get observedAttributes() { return ['role', 'content']; }
  connectedCallback() { this.render(); }
  attributeChangedCallback() { this.render(); }
  appendDelta(text) {
    // 流式追加:直接拼到 .ace-msg-body
    const body = this.querySelector('.ace-msg-body');
    if (body) body.textContent += text;
    else this.setAttribute('content', (this.getAttribute('content') || '') + text);
  }
  render() {
    const role    = this.getAttribute('role')    || 'system';
    const content = this.getAttribute('content') || '';
    const cls = role === 'user' ? 'ace-msg-user'
              : role === 'assistant' ? 'ace-msg-assistant'
              : role === 'tool' || role === 'tool_call' || role === 'tool_result' ? 'ace-msg-tool'
              : 'ace-msg-system';
    this.className = `${cls} p-2 my-1 rounded`;
    // textContent 不解析 HTML,安全。markdown 后续再渲染。
    this.innerHTML = `<div class="ace-msg-body" style="white-space:pre-wrap;font-family:inherit"></div>`;
    this.querySelector('.ace-msg-body').textContent = content;
  }
}
customElements.define('ace-message', AceMessage);
