// 桌面 UI 形态检测:WebView2 壳(shell) / Edge app 兼容模式(webapp) / 普通浏览器(browser)。
//
// webapp 兼容模式由桌面壳以 `msedge --app=<url>?ace_webapp=1` 启动(src/desktop/main.cpp
// 的 run_edge_app_mode)。App 内多处 history.replaceState(抹 ?token= / ?open=)和整页
// navigate 会重建 URL,query 参数活不过一次跳转,所以首次见到 ace_webapp=1 就固化到
// sessionStorage(per-tab,与 lib/auth.js 处理 ?token= 同模式)。

export const WEBAPP_COMPAT_QUERY_KEY = 'ace_webapp';
export const WEBAPP_COMPAT_STORAGE_KEY = 'ace.webappCompat';

function safeSessionStorage(win) {
  try {
    return win?.sessionStorage || null;
  } catch {
    return null; // 某些隐私模式下访问 sessionStorage 本身会抛
  }
}

// 纯函数:从 location.search 与已固化的 storage 值判断是否处于 webapp 兼容模式。
export function detectWebappCompat({ search = '', storedFlag = '' } = {}) {
  if (storedFlag === '1') return true;
  try {
    return new URLSearchParams(search || '').get(WEBAPP_COMPAT_QUERY_KEY) === '1';
  } catch {
    return false;
  }
}

// 启动期调用一次(main.jsx,React mount 之前):检测 + 固化 + 暴露全局标志。
// 返回是否处于 webapp 兼容模式。
export function installWebappCompatFlag(win = typeof window !== 'undefined' ? window : undefined) {
  if (!win) return false;
  const storage = safeSessionStorage(win);
  let storedFlag = '';
  try {
    storedFlag = storage?.getItem(WEBAPP_COMPAT_STORAGE_KEY) || '';
  } catch { /* 读失败按未固化处理 */ }
  const compat = detectWebappCompat({ search: win.location?.search || '', storedFlag });
  if (compat) {
    win.__ACECODE_WEBAPP_COMPAT__ = true;
    try {
      storage?.setItem(WEBAPP_COMPAT_STORAGE_KEY, '1');
    } catch { /* 写失败静默:本次会话仍有全局标志 */ }
  }
  return compat;
}

export function isWebappCompat(win = typeof window !== 'undefined' ? window : undefined) {
  if (!win) return false;
  if (win.__ACECODE_WEBAPP_COMPAT__) return true;
  let storedFlag = '';
  try {
    storedFlag = safeSessionStorage(win)?.getItem(WEBAPP_COMPAT_STORAGE_KEY) || '';
  } catch { /* ignore */ }
  return detectWebappCompat({ search: win.location?.search || '', storedFlag });
}

export function isDesktopShell(win = typeof window !== 'undefined' ? window : undefined) {
  if (!win) return false;
  return !!(win.__ACECODE_DESKTOP_SHELL__ || win.aceDesktop_openDevTools || win.aceDesktop_openInExplorer);
}

// 'shell' = WebView2 桌面壳;'webapp' = Edge app 兼容模式;'browser' = 普通浏览器直连。
export function desktopUiMode(win = typeof window !== 'undefined' ? window : undefined) {
  if (isDesktopShell(win)) return 'shell';
  if (isWebappCompat(win)) return 'webapp';
  return 'browser';
}
