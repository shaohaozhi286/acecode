export function normalizePinnedIds(ids = []) {
  const out = [];
  const seen = new Set();
  for (const raw of Array.isArray(ids) ? ids : []) {
    const id = String(raw || '');
    if (!id || seen.has(id)) continue;
    seen.add(id);
    out.push(id);
  }
  return out;
}

export function pinSessionId(ids = [], sessionId = '') {
  const id = String(sessionId || '');
  const current = normalizePinnedIds(ids).filter((item) => item !== id);
  return id ? [id, ...current] : current;
}

export function unpinSessionId(ids = [], sessionId = '') {
  const id = String(sessionId || '');
  return normalizePinnedIds(ids).filter((item) => item !== id);
}

function workspaceEntries(pinnedByWorkspace) {
  if (pinnedByWorkspace instanceof Map) return Array.from(pinnedByWorkspace.entries());
  if (pinnedByWorkspace && typeof pinnedByWorkspace === 'object') return Object.entries(pinnedByWorkspace);
  return [];
}

export function pinnedIdSet(pinnedByWorkspace) {
  const out = new Set();
  for (const [, ids] of workspaceEntries(pinnedByWorkspace)) {
    for (const id of normalizePinnedIds(ids)) out.add(id);
  }
  return out;
}

export function pinnedSessionsForList(sessions = [], pinnedByWorkspace) {
  const byWorkspaceAndId = new Map();
  for (const session of Array.isArray(sessions) ? sessions : []) {
    const id = session?.id || session?.session_id || '';
    const workspaceHash = session?.workspace_hash || session?.workspaceHash || '';
    if (!id) continue;
    byWorkspaceAndId.set(`${workspaceHash}\u0000${id}`, session);
  }

  const out = [];
  const emitted = new Set();
  for (const [workspaceHash, ids] of workspaceEntries(pinnedByWorkspace)) {
    for (const id of normalizePinnedIds(ids)) {
      const key = `${workspaceHash}\u0000${id}`;
      if (emitted.has(key)) continue;
      const session = byWorkspaceAndId.get(key);
      if (!session) continue;
      emitted.add(key);
      out.push({ ...session, pinned: true });
    }
  }
  return out;
}

export function filterPinnedSessions(sessions = [], pinnedByWorkspace) {
  const pinned = pinnedIdSet(pinnedByWorkspace);
  return (Array.isArray(sessions) ? sessions : []).filter((session) => {
    const id = session?.id || session?.session_id || '';
    return !id || !pinned.has(id);
  });
}
