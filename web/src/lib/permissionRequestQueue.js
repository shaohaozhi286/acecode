// Permission request lifecycle for Desktop/Web.
//
// Entries are keyed by request_id and stay ordered by first arrival:
//   pending -> submitting -> resolved
//
// A resolved tombstone may arrive before its replayed permission_request. Keep
// that ID until the owning turn terminates so a duplicate request can never
// resurrect actionable controls.

export const PERMISSION_REQUEST_STATUS = Object.freeze({
  PENDING: 'pending',
  SUBMITTING: 'submitting',
  RESOLVED: 'resolved',
});

function requestId(payload) {
  return String(payload?.request_id || '');
}

function replaceAt(list, index, entry) {
  const next = list.slice();
  next[index] = entry;
  return next;
}

export function pushPermissionRequest(list, payload, { ownerSessionId = '' } = {}) {
  const id = requestId(payload);
  if (!id) return list;

  const index = list.findIndex((entry) => entry.request_id === id);
  if (index < 0) {
    return [...list, {
      ...payload,
      request_id: id,
      owner_session_id: ownerSessionId || payload.owner_session_id || '',
      status: PERMISSION_REQUEST_STATUS.PENDING,
      has_request: true,
    }];
  }

  const previous = list[index];
  const next = {
    ...previous,
    ...payload,
    request_id: id,
    owner_session_id: ownerSessionId
      || previous.owner_session_id
      || payload.owner_session_id
      || '',
    // A replayed request supplies presentation data but never reopens a
    // submitting/resolved entry.
    status: previous.status,
    has_request: true,
  };
  return replaceAt(list, index, next);
}

export function markPermissionSubmitting(list, requestIdValue, choice) {
  const id = String(requestIdValue || '');
  if (!id) return list;
  const index = list.findIndex((entry) => entry.request_id === id);
  if (index < 0 || list[index].status !== PERMISSION_REQUEST_STATUS.PENDING) return list;
  return replaceAt(list, index, {
    ...list[index],
    status: PERMISSION_REQUEST_STATUS.SUBMITTING,
    submitted_choice: choice,
  });
}

export function closePermissionRequest(list, payload, { ownerSessionId = '' } = {}) {
  const id = requestId(payload);
  if (!id) return list;

  const index = list.findIndex((entry) => entry.request_id === id);
  if (index < 0) {
    return [...list, {
      ...payload,
      request_id: id,
      owner_session_id: ownerSessionId || payload.owner_session_id || '',
      status: PERMISSION_REQUEST_STATUS.RESOLVED,
      has_request: false,
    }];
  }

  const previous = list[index];
  if (previous.status === PERMISSION_REQUEST_STATUS.RESOLVED) return list;
  return replaceAt(list, index, {
    ...previous,
    ...payload,
    request_id: id,
    owner_session_id: ownerSessionId
      || previous.owner_session_id
      || payload.owner_session_id
      || '',
    status: PERMISSION_REQUEST_STATUS.RESOLVED,
    has_request: previous.has_request !== false,
  });
}

export function clearResolvedPermissionRequests(list, sessionId) {
  const sid = String(sessionId || '');
  if (!sid) return list;
  const next = list.filter((entry) => !(
    entry.status === PERMISSION_REQUEST_STATUS.RESOLVED
    && (entry.session_id === sid || (!entry.session_id && entry.owner_session_id === sid))
  ));
  return next.length === list.length ? list : next;
}

export function permissionOwnerSessionId(entry, ownership = {}, fallbackSessionId = '') {
  const sessionId = String(entry?.session_id || fallbackSessionId || '');
  if (entry?.owner_session_id) return String(entry.owner_session_id);
  if (sessionId && ownership?.owners?.[sessionId]) {
    return String(ownership.owners[sessionId]);
  }
  if (sessionId
      && ownership?.parentId
      && ownership?.titles?.[sessionId]) {
    return String(ownership.parentId);
  }
  return sessionId;
}

export function visiblePermissionRequests(list, activeSessionId, ownership = {}) {
  const activeId = String(activeSessionId || '');
  if (!activeId) return [];

  let unresolvedIncluded = false;
  return list.filter((entry) => {
    if (!entry?.has_request) return false;
    if (permissionOwnerSessionId(entry, ownership) !== activeId) return false;
    if (entry.status === PERMISSION_REQUEST_STATUS.RESOLVED) return true;
    if (unresolvedIncluded) return false;
    unresolvedIncluded = true;
    return true;
  });
}

export function hasUnresolvedPermission(list) {
  return list.some((entry) => (
    entry?.status === PERMISSION_REQUEST_STATUS.PENDING
    || entry?.status === PERMISSION_REQUEST_STATUS.SUBMITTING
  ));
}

export function pendingPermissionSessionIds(list, fallbackSessionId = '', ownership = {}) {
  const ids = new Set();
  for (const entry of list) {
    if (entry?.status !== PERMISSION_REQUEST_STATUS.PENDING
        && entry?.status !== PERMISSION_REQUEST_STATUS.SUBMITTING) {
      continue;
    }
    const owner = permissionOwnerSessionId(entry, ownership, fallbackSessionId);
    if (owner) ids.add(owner);
  }
  return ids;
}

export function sessionHasPendingPermission(session, sessionIds) {
  return !!session?.id && sessionIds instanceof Set && sessionIds.has(session.id);
}

export function permissionOriginLabel(entry, ownership = {}) {
  const sessionId = String(entry?.session_id || '');
  const ownerSessionId = permissionOwnerSessionId(entry, ownership);
  if (!sessionId || !ownerSessionId || sessionId === ownerSessionId) return '';
  const title = ownership?.titles?.[sessionId] || sessionId;
  return `来自后台任务:${title}`;
}
