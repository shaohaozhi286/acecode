const STATES = new Set(['read', 'unread', 'in_progress']);

export function normalizeAttentionState(value, fallback = 'read') {
  const state = typeof value === 'string' ? value : '';
  if (STATES.has(state)) return state;
  return STATES.has(fallback) ? fallback : 'read';
}

export function sessionAttentionState(session = {}) {
  if (session.busy || session.status === 'running') return 'in_progress';
  return normalizeAttentionState(session.attention_state || session.read_state || session.state, 'read');
}

export function statusCursor(status = {}) {
  return Number(status.cursor ?? status.status_cursor ?? status.update_cursor ?? 0) || 0;
}

export function normalizeStatusPayload(payload = {}) {
  const sessionId = payload.session_id || payload.id || '';
  if (!sessionId) return null;
  const busy = !!payload.busy || payload.state === 'in_progress' || payload.attention_state === 'in_progress';
  const state = busy
    ? 'in_progress'
    : normalizeAttentionState(payload.state || payload.attention_state || payload.read_state, 'read');
  const cursor = statusCursor(payload);
  const updateCursor = Number(payload.update_cursor ?? cursor) || 0;
  const readCursor = Number(payload.read_cursor ?? 0) || 0;
  return {
    session_id: sessionId,
    id: sessionId,
    workspace_hash: payload.workspace_hash || '',
    cwd: payload.cwd || '',
    state,
    attention_state: state,
    read_state: state,
    status: busy ? 'running' : 'idle',
    busy,
    cursor,
    status_cursor: cursor,
    update_cursor: updateCursor,
    read_cursor: readCursor,
    timestamp_ms: Number(payload.timestamp_ms ?? 0) || 0,
  };
}

export function applyStatusUpdate(statusMap, payload) {
  const next = new Map(statusMap || []);
  const normalized = normalizeStatusPayload(payload);
  if (!normalized) return next;
  const prev = next.get(normalized.session_id);
  const prevTs = Number(prev?.timestamp_ms ?? 0) || 0;
  if (prev && normalized.timestamp_ms && prevTs > normalized.timestamp_ms) return next;
  next.set(normalized.session_id, { ...prev, ...normalized });
  return next;
}

export function applyStatusSnapshot(statusMap, payload = {}) {
  let next = new Map(statusMap || []);
  const sessions = Array.isArray(payload.sessions) ? payload.sessions : [];
  for (const item of sessions) next = applyStatusUpdate(next, item);
  return next;
}

export function mergeSessionStatus(session = {}, statusMap) {
  const id = session.id || session.session_id || '';
  const status = id && statusMap ? statusMap.get(id) : null;
  const merged = status ? { ...session, ...status, id } : { ...session, id };
  const state = sessionAttentionState(merged);
  return {
    ...merged,
    attention_state: state,
    read_state: state,
    status_cursor: statusCursor(merged),
  };
}

export function mergeSessionsWithStatus(sessions = [], statusMap) {
  return sessions.map((session) => mergeSessionStatus(session, statusMap));
}

export function workspaceHasUnread(sessions = []) {
  return sessions.some((session) => sessionAttentionState(session) === 'unread');
}

export function optimisticReadStatus(session = {}) {
  const state = sessionAttentionState(session);
  if (state === 'in_progress') return normalizeStatusPayload({ ...session, state });
  const cursor = statusCursor(session);
  return normalizeStatusPayload({
    ...session,
    state: 'read',
    attention_state: 'read',
    read_state: 'read',
    busy: false,
    read_cursor: cursor,
    cursor,
    update_cursor: cursor,
    timestamp_ms: Date.now(),
  });
}
