// 主入口:动态注册组件 + 探活 + 渲染。

import { api, AceApiError } from './api.js';
import { setToken } from './auth.js';

// 注册所有 Web Components(顺序无关,Custom Elements 可前向引用)
import './components/ace-token-prompt.js';
import './components/ace-message.js';
import './components/ace-tool-block.js';
import './components/ace-permission-modal.js';
import './components/ace-question-modal.js';
import './components/ace-model-picker.js';
import './components/ace-sidebar.js';
import './components/ace-skills-panel.js';
import './components/ace-mcp-editor.js';
import './components/ace-chat.js';
import './components/ace-app.js';

// 简易事件总线 — 组件之间通过 window 事件松耦合通信
window.AceBus = window.AceBus || new EventTarget();

// 启动后立刻探活 — 401 弹 token-prompt
async function bootstrap() {
  try {
    const h = await api.health();
    window.AceBus.dispatchEvent(new CustomEvent('health', { detail: h }));
  } catch (e) {
    if (e instanceof AceApiError && e.status === 401) {
      window.AceBus.dispatchEvent(new Event('need-token'));
    } else {
      console.error('health probe failed', e);
    }
  }
}

window.AceTokenSubmit = async function(token) {
  setToken(token);
  await bootstrap();
};

// 等 DOM Ready 后启动 — module 默认 defer 但保险起见。
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', bootstrap);
} else {
  bootstrap();
}
