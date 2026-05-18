export const SIDEBAR_SESSION_COLLAPSE_LIMIT = 5;

function sessionId(session) {
  return String(session?.id || session?.session_id || session?.sessionId || '').trim();
}

function sessionTime(session) {
  const s = session || {};
  const updated = Date.parse(s.updated_at || s.updatedAt || '');
  if (Number.isFinite(updated)) return updated;
  const created = Date.parse(s.created_at || s.createdAt || '');
  if (Number.isFinite(created)) return created;
  return 0;
}

export function sidebarSessionProjection(sessions = [], expanded = false, limit = SIDEBAR_SESSION_COLLAPSE_LIMIT) {
  const list = Array.isArray(sessions) ? sessions : [];
  const max = Number.isFinite(limit) && limit > 0 ? Math.floor(limit) : SIDEBAR_SESSION_COLLAPSE_LIMIT;
  const collapsible = list.length > max;
  const visibleSessions = collapsible && !expanded ? list.slice(0, max) : list;
  return {
    visibleSessions,
    collapsible,
    action: collapsible ? (expanded ? 'collapse' : 'expand') : '',
    hiddenCount: collapsible && !expanded ? list.length - max : 0,
  };
}

export function sortSidebarSessionsNewestFirst(sessions = []) {
  const list = Array.isArray(sessions) ? sessions : [];
  return list
    .map((session, index) => ({ session, index, time: sessionTime(session) }))
    .sort((a, b) => {
      if (a.time !== b.time) return b.time - a.time;
      return a.index - b.index;
    })
    .map((item) => item.session);
}

export function upsertSidebarSession(sessions = [], nextSession = null) {
  const id = sessionId(nextSession);
  if (!id) return Array.isArray(sessions) ? sessions : [];

  const list = Array.isArray(sessions) ? sessions : [];
  let replaced = false;
  const merged = list.map((session) => {
    if (sessionId(session) !== id) return session;
    replaced = true;
    return { ...session, ...nextSession, id };
  });
  if (!replaced) merged.push({ ...nextSession, id });
  return sortSidebarSessionsNewestFirst(merged);
}
