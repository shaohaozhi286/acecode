// Token 持久化:URL ?token=…(初次进入)→ sessionStorage,后续 API 与 WS 都从这里读。
//
// 跨浏览器标签同源时 sessionStorage 是隔离的,刚好契合"每个 desktop window
// 独立 daemon 端口"的模型;不漂洗到不必要的标签。

const STORAGE_KEY = 'ace.token';

export function getToken() {
  // URL 优先: desktop / re-launch 链接里 ?token=… 写到当前 tab 的 storage
  const url = new URL(window.location.href);
  const fromUrl = url.searchParams.get('token');
  if (fromUrl) {
    sessionStorage.setItem(STORAGE_KEY, fromUrl);
    url.searchParams.delete('token');
    window.history.replaceState({}, '', url.toString());
    return fromUrl;
  }
  return sessionStorage.getItem(STORAGE_KEY) || '';
}

export function setToken(t) {
  if (t) sessionStorage.setItem(STORAGE_KEY, t);
  else   sessionStorage.removeItem(STORAGE_KEY);
}

export function clearToken() { sessionStorage.removeItem(STORAGE_KEY); }
