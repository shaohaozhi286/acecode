// 应用级通知 session 订阅与事件去重。
//
// ChatView 离开时会 release 自己的 connection 引用；通知 monitor 对运行中
// session 再持有一份引用，确保后台完成/权限/提问事件仍能到达 App。

export function createDesktopNotificationMonitor({
  retainSession,
  releaseSession,
  maxSeen = 512,
} = {}) {
  const retained = new Set();
  const seen = new Set();

  const trimSeen = () => {
    while (seen.size > maxSeen) {
      const oldest = seen.values().next().value;
      if (oldest === undefined) break;
      seen.delete(oldest);
    }
  };

  return {
    retain(sessionId) {
      const sid = String(sessionId || '');
      if (!sid || retained.has(sid)) return false;
      retained.add(sid);
      retainSession?.(sid);
      return true;
    },

    release(sessionId) {
      const sid = String(sessionId || '');
      if (!sid || !retained.delete(sid)) return false;
      releaseSession?.(sid);
      return true;
    },

    markSeen(key) {
      const normalized = String(key || '');
      if (!normalized || seen.has(normalized)) return false;
      seen.add(normalized);
      trimSeen();
      return true;
    },

    isRetained(sessionId) {
      return retained.has(String(sessionId || ''));
    },

    dispose() {
      for (const sid of retained) releaseSession?.(sid);
      retained.clear();
      seen.clear();
    },
  };
}

export function notificationEventKey(type, sessionId, message = {}, payload = {}) {
  const requestId = payload.request_id || message.request_id || '';
  if (requestId) return `${type}:request:${requestId}`;
  const seq = Number.isFinite(message.seq) ? message.seq : '';
  if (seq !== '') return `${type}:session:${sessionId || ''}:seq:${seq}`;
  const timestamp = message.timestamp_ms || payload.timestamp_ms || '';
  return `${type}:session:${sessionId || ''}:time:${timestamp}`;
}
