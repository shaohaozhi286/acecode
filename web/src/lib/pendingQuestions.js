// AskUserQuestion request lifecycle for Desktop/Web.
//
// Entries are keyed by request_id and stay ordered by first arrival:
//   pending -> resolved
//
// A seq-less subscription snapshot may race with a newer question_closed
// event. Resolved entries therefore remain as tombstones until the owning turn
// terminates, preventing an older replay from reopening the picker.

export const QUESTION_REQUEST_STATUS = Object.freeze({
  PENDING: 'pending',
  RESOLVED: 'resolved',
});

function requestId(payload) {
  return String(payload?.request_id || '').trim();
}

function replaceAt(list, index, entry) {
  const next = list.slice();
  next[index] = entry;
  return next;
}

function requestStatus(entry) {
  return entry?.status === QUESTION_REQUEST_STATUS.RESOLVED
    ? QUESTION_REQUEST_STATUS.RESOLVED
    : QUESTION_REQUEST_STATUS.PENDING;
}

export function addPendingQuestionRequest(
  requests = [],
  payload = {},
  { ownerSessionId = '' } = {},
) {
  const list = Array.isArray(requests) ? requests : [];
  const id = requestId(payload);
  if (!id) return list;

  const index = list.findIndex((entry) => requestId(entry) === id);
  if (index < 0) {
    return [...list, {
      ...payload,
      request_id: id,
      owner_session_id: ownerSessionId || payload.owner_session_id || '',
      status: QUESTION_REQUEST_STATUS.PENDING,
      has_request: true,
    }];
  }

  const previous = list[index];
  return replaceAt(list, index, {
    ...previous,
    ...payload,
    request_id: id,
    owner_session_id: ownerSessionId
      || previous.owner_session_id
      || payload.owner_session_id
      || '',
    // Replayed snapshots add presentation data but never reopen a resolved
    // question.
    status: requestStatus(previous),
    has_request: true,
  });
}

export function closePendingQuestionRequest(
  requests = [],
  payload = {},
  { ownerSessionId = '' } = {},
) {
  const list = Array.isArray(requests) ? requests : [];
  const normalizedPayload = typeof payload === 'string'
    ? { request_id: payload }
    : payload;
  const id = requestId(normalizedPayload);
  if (!id) return list;

  const index = list.findIndex((entry) => requestId(entry) === id);
  if (index < 0) {
    return [...list, {
      ...normalizedPayload,
      request_id: id,
      owner_session_id: ownerSessionId || normalizedPayload.owner_session_id || '',
      status: QUESTION_REQUEST_STATUS.RESOLVED,
      has_request: false,
    }];
  }

  const previous = list[index];
  if (requestStatus(previous) === QUESTION_REQUEST_STATUS.RESOLVED) return list;
  return replaceAt(list, index, {
    ...previous,
    ...normalizedPayload,
    request_id: id,
    owner_session_id: ownerSessionId
      || previous.owner_session_id
      || normalizedPayload.owner_session_id
      || '',
    status: QUESTION_REQUEST_STATUS.RESOLVED,
    has_request: previous.has_request !== false,
  });
}

export function clearResolvedQuestionRequests(requests = [], sessionId = '') {
  const list = Array.isArray(requests) ? requests : [];
  const sid = String(sessionId || '').trim();
  if (!sid) return list;
  const next = list.filter((entry) => !(
    requestStatus(entry) === QUESTION_REQUEST_STATUS.RESOLVED
    && (entry.session_id === sid || (!entry.session_id && entry.owner_session_id === sid))
  ));
  return next.length === list.length ? list : next;
}

export function removePendingQuestionRequest(requests = [], requestId = '') {
  const list = Array.isArray(requests) ? requests : [];
  const id = String(requestId || '').trim();
  if (!id) return list;
  const next = list.filter((request) => request?.request_id !== id);
  return next.length === list.length ? list : next;
}

export function questionOwnerSessionId(entry, ownership = {}, fallbackSessionId = '') {
  const sessionId = String(entry?.session_id || entry?.sessionId || fallbackSessionId || '').trim();
  if (entry?.owner_session_id) return String(entry.owner_session_id);
  if (sessionId && ownership?.owners?.[sessionId]) {
    return String(ownership.owners[sessionId]);
  }
  if (sessionId && ownership?.parentId && ownership?.titles?.[sessionId]) {
    return String(ownership.parentId);
  }
  return sessionId;
}

export function visibleQuestionRequest(requests = [], activeSessionId = '', ownership = {}) {
  const activeId = String(activeSessionId || '').trim();
  if (!activeId) return null;
  const list = Array.isArray(requests) ? requests : [];
  return list.find((entry) => (
    entry?.has_request !== false
    && requestStatus(entry) === QUESTION_REQUEST_STATUS.PENDING
    && questionOwnerSessionId(entry, ownership) === activeId
  )) || null;
}

export function pendingQuestionSessionIds(
  requests = [],
  fallbackSessionId = '',
  ownership = {},
) {
  const fallback = String(fallbackSessionId || '').trim();
  const ids = new Set();
  const list = Array.isArray(requests) ? requests : [];
  for (const request of list) {
    if (!request?.request_id) continue;
    if (request?.has_request === false
        || requestStatus(request) === QUESTION_REQUEST_STATUS.RESOLVED) {
      continue;
    }
    const id = questionOwnerSessionId(request, ownership, fallback);
    if (id) ids.add(id);
  }
  return ids;
}

export function sessionHasPendingQuestion(session = {}, pendingIds = new Set()) {
  const id = String(session?.id || session?.session_id || session?.sessionId || '').trim();
  return !!id && !!pendingIds?.has?.(id);
}

export function questionOriginLabel(entry, ownership = {}) {
  const sessionId = String(entry?.session_id || entry?.sessionId || '').trim();
  const ownerSessionId = questionOwnerSessionId(entry, ownership);
  if (!sessionId || !ownerSessionId || sessionId === ownerSessionId) return '';
  const title = ownership?.titles?.[sessionId] || sessionId;
  return `来自后台任务:${title}`;
}
