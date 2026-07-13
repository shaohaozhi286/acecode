export const CHANGE_DOCK_DISMISSALS_STORAGE_KEY = 'acecode.changeDockDismissals.v1';

const MAX_DISMISSED_DOCK_ENTRIES = 200;

function isRecord(value) {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

export function validateDockDismissals(value) {
  if (!isRecord(value)) return false;
  return Object.entries(value).every(([key, signature]) => (
    typeof key === 'string'
    && key.length > 0
    && typeof signature === 'string'
    && signature.length > 0
  ));
}

function normalizedDockDismissals(value) {
  if (!isRecord(value)) return {};
  const out = {};
  for (const [key, signature] of Object.entries(value)) {
    if (typeof key === 'string' && key && typeof signature === 'string' && signature) {
      out[key] = signature;
    }
  }
  return out;
}

export function dockDismissalKey(sessionRef, sessionId = '') {
  const sid = String(sessionId || sessionRef?.sessionId || '');
  if (!sid) return '';
  const workspaceHash = String(sessionRef?.workspaceHash || '');
  return workspaceHash ? `${workspaceHash}:${sid}` : sid;
}

export function dismissedDockSignatureFor(dismissals, key) {
  if (!key || !isRecord(dismissals)) return '';
  const value = dismissals[key];
  return typeof value === 'string' ? value : '';
}

// —— 下一轮提交时的整体自动收起(变更 + todo)——
// 变更部分复用上面的签名 dismissal(新变更签名变化后 dock 自动重现);
// todo 部分是会话内存级抑制:提交时记住当时的 todo 快照签名,快照没变
// 就不给 dock 传 todos,agent 在新回合里更新 todo(签名变化)后进度环
// 自动重现。todos 数组与 summary 都来自同一 WS payload,键序稳定,
// JSON.stringify 足够当签名用。
export function todoDockSignature(todos, todoSummary) {
  try {
    return JSON.stringify([Array.isArray(todos) ? todos : [], todoSummary ?? null]);
  } catch {
    return '';
  }
}

export function isTodoDockSuppressed(state, sessionKey, signature) {
  return !!state
    && !!sessionKey
    && state.sessionKey === sessionKey
    && !!signature
    && state.signature === signature;
}

export function dismissChangeDockSignature(dismissals, key, signature) {
  if (!key || !signature) return normalizedDockDismissals(dismissals);
  const next = normalizedDockDismissals(dismissals);
  delete next[key];
  next[key] = signature;

  const keys = Object.keys(next);
  if (keys.length <= MAX_DISMISSED_DOCK_ENTRIES) return next;

  const trimmed = { ...next };
  for (const staleKey of keys.slice(0, keys.length - MAX_DISMISSED_DOCK_ENTRIES)) {
    delete trimmed[staleKey];
  }
  return trimmed;
}