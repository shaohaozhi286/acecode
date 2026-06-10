// ConsoleDock 纯函数层(openspec/changes/add-console-dock,specs/console-dock-ui)。
// tab 状态机 / WS 帧分流 / 重连退避 / 高度 clamp / WS URL 构建。
// DOM/xterm/WebSocket 接线在 components/ConsoleDock.jsx;这里只做可单测的数据塑形。

// ---------------------------------------------------------------------------
// tab 状态机:{tabs: [{id, title, status, exitCode}], activeId}
// ---------------------------------------------------------------------------

export function createDockTabs() {
  return { tabs: [], activeId: '' };
}

export function addTab(state, info) {
  const tab = {
    id: info.id,
    title: info.title || info.id,
    status: info.status || 'running',
    exitCode: typeof info.exit_code === 'number' ? info.exit_code : null,
    backend: info.backend || '',
  };
  return { tabs: [...state.tabs, tab], activeId: tab.id };
}

export function removeTab(state, id) {
  const tabs = state.tabs.filter((t) => t.id !== id);
  let activeId = state.activeId;
  if (activeId === id) {
    // 关闭当前激活 tab:激活右邻(原位置的下一个),没有则左邻,空则清空。
    const idx = state.tabs.findIndex((t) => t.id === id);
    const next = tabs[Math.min(idx, tabs.length - 1)];
    activeId = next ? next.id : '';
  }
  return { tabs, activeId };
}

export function activateTab(state, id) {
  if (!state.tabs.some((t) => t.id === id)) return state;
  return { ...state, activeId: id };
}

export function markTabExited(state, id, exitCode) {
  const tabs = state.tabs.map((t) =>
    t.id === id ? { ...t, status: 'exited', exitCode } : t,
  );
  return { ...state, tabs };
}

// 终端内程序经 OSC 0/2 改标题(xterm onTitleChange 透传)。空白标题忽略
// (保留 "Terminal N" 默认);超长截断防 tab 栏被撑爆。
export function renameTab(state, id, title) {
  const trimmed = String(title || '').trim().slice(0, 200);
  if (!trimmed) return state;
  if (!state.tabs.some((t) => t.id === id)) return state;
  const tabs = state.tabs.map((t) =>
    t.id === id ? { ...t, title: trimmed } : t,
  );
  return { ...state, tabs };
}

// ---------------------------------------------------------------------------
// WS 帧分流:服务端全部走 binary 帧;首字节 0x00 = UTF-8 JSON 控制帧
// (cursor 同步 / exit 通知),其余为原始 PTY 字节流(直接喂 xterm)。
// ---------------------------------------------------------------------------

export function parsePtyFrame(arrayBuffer) {
  const bytes = new Uint8Array(arrayBuffer);
  if (bytes.length > 0 && bytes[0] === 0) {
    try {
      const json = JSON.parse(new TextDecoder().decode(bytes.subarray(1)));
      return { kind: 'control', payload: json };
    } catch {
      return { kind: 'control', payload: null };
    }
  }
  return { kind: 'data', bytes };
}

// ---------------------------------------------------------------------------
// 重连退避(opencode 同款节奏:250ms × 2^n,上限 4s)
// ---------------------------------------------------------------------------

export function nextReconnectDelay(tries) {
  const n = Math.max(0, Math.min(Number(tries) || 0, 4));
  return Math.min(250 * 2 ** n, 4000);
}

// ---------------------------------------------------------------------------
// dock 高度 clamp:最小可用高度 ~ 视口的 80%(聊天区永远留 20%)。
// ---------------------------------------------------------------------------

export const CONSOLE_DOCK_MIN_HEIGHT = 120;
export const CONSOLE_DOCK_DEFAULT_HEIGHT = 280;

export function clampDockHeight(height, viewportHeight) {
  const h = Number.isFinite(height) ? height : CONSOLE_DOCK_DEFAULT_HEIGHT;
  const vh = Number.isFinite(viewportHeight) && viewportHeight > 0
    ? viewportHeight : 900;
  const max = Math.max(CONSOLE_DOCK_MIN_HEIGHT, Math.floor(vh * 0.8));
  return Math.min(Math.max(Math.round(h), CONSOLE_DOCK_MIN_HEIGHT), max);
}

// ---------------------------------------------------------------------------
// WS URL:/ws/pty/<id>?cursor=N[&token=...]。
// origin 形如 "http://127.0.0.1:28080" 或空(同源)。
// ---------------------------------------------------------------------------

export function ptyWsUrl({ id, cursor = -1, token = '', origin = '', pageProtocol = 'http:', pageHost = '' }) {
  let proto;
  let host;
  if (origin) {
    proto = origin.startsWith('https') ? 'wss:' : 'ws:';
    host = origin.replace(/^https?:\/\//, '');
  } else {
    proto = pageProtocol === 'https:' ? 'wss:' : 'ws:';
    host = pageHost;
  }
  const qs = new URLSearchParams();
  qs.set('cursor', String(Math.trunc(Number.isFinite(cursor) ? cursor : -1)));
  if (token) qs.set('token', token);
  return `${proto}//${host}/ws/pty/${encodeURIComponent(id)}?${qs.toString()}`;
}
