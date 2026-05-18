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
