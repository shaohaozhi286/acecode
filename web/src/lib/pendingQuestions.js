export function addPendingQuestionRequest(requests = [], payload = {}) {
  if (!payload?.request_id) return Array.isArray(requests) ? requests : [];
  const list = Array.isArray(requests) ? requests : [];
  return list.some((request) => request?.request_id === payload.request_id)
    ? list
    : [...list, payload];
}

export function removePendingQuestionRequest(requests = [], requestId = '') {
  const list = Array.isArray(requests) ? requests : [];
  const id = String(requestId || '').trim();
  if (!id) return list;
  const next = list.filter((request) => request?.request_id !== id);
  return next.length === list.length ? list : next;
}

export function pendingQuestionSessionIds(requests = [], fallbackSessionId = '') {
  const fallback = String(fallbackSessionId || '').trim();
  const ids = new Set();
  const list = Array.isArray(requests) ? requests : [];
  for (const request of list) {
    if (!request?.request_id) continue;
    const id = String(request.session_id || request.sessionId || '').trim() || fallback;
    if (id) ids.add(id);
  }
  return ids;
}

export function sessionHasPendingQuestion(session = {}, pendingIds = new Set()) {
  const id = String(session?.id || session?.session_id || session?.sessionId || '').trim();
  return !!id && !!pendingIds?.has?.(id);
}
