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

export function pinnedOrderKey(workspaceHash = '', sessionId = '') {
  const ws = String(workspaceHash || '');
  const id = String(sessionId || '');
  return ws && id ? `${ws}\u0000${id}` : '';
}

export function normalizePinnedOrderItems(items = []) {
  const out = [];
  const seen = new Set();
  for (const raw of Array.isArray(items) ? items : []) {
    const workspaceHash = String(raw?.workspace_hash || raw?.workspaceHash || '');
    const sessionId = String(raw?.session_id || raw?.sessionId || raw?.id || '');
    const key = pinnedOrderKey(workspaceHash, sessionId);
    if (!key || seen.has(key)) continue;
    seen.add(key);
    out.push({ workspace_hash: workspaceHash, session_id: sessionId });
  }
  return out;
}

export function pinnedOrderItemsForSessions(sessions = []) {
  return normalizePinnedOrderItems((Array.isArray(sessions) ? sessions : []).map((session) => ({
    workspace_hash: session?.workspace_hash || session?.workspaceHash || '',
    session_id: session?.id || session?.session_id || session?.sessionId || '',
  })));
}

export function reorderPinnedOrderItems(items = [], source = {}, target = {}, placement = 'before') {
  const current = normalizePinnedOrderItems(items);
  const sourceKey = pinnedOrderKey(source?.workspace_hash || source?.workspaceHash, source?.session_id || source?.sessionId || source?.id);
  const targetKey = pinnedOrderKey(target?.workspace_hash || target?.workspaceHash, target?.session_id || target?.sessionId || target?.id);
  if (!sourceKey || !targetKey || sourceKey === targetKey) return current;
  if (!current.some((item) => pinnedOrderKey(item.workspace_hash, item.session_id) === sourceKey)) return current;
  if (!current.some((item) => pinnedOrderKey(item.workspace_hash, item.session_id) === targetKey)) return current;

  const withoutSource = current.filter((item) => pinnedOrderKey(item.workspace_hash, item.session_id) !== sourceKey);
  const targetIndex = withoutSource.findIndex((item) => pinnedOrderKey(item.workspace_hash, item.session_id) === targetKey);
  if (targetIndex < 0) return current;
  const sourceItem = current.find((item) => pinnedOrderKey(item.workspace_hash, item.session_id) === sourceKey);
  const insertIndex = placement === 'after' ? targetIndex + 1 : targetIndex;
  return [
    ...withoutSource.slice(0, insertIndex),
    sourceItem,
    ...withoutSource.slice(insertIndex),
  ];
}

export function pinPinnedOrderItem(items = [], item = {}) {
  const normalized = normalizePinnedOrderItems(items);
  const next = normalizePinnedOrderItems([item]);
  if (next.length === 0) return normalized;
  const key = pinnedOrderKey(next[0].workspace_hash, next[0].session_id);
  return [next[0], ...normalized.filter((entry) => pinnedOrderKey(entry.workspace_hash, entry.session_id) !== key)];
}

export function unpinPinnedOrderItem(items = [], item = {}) {
  const key = pinnedOrderKey(item?.workspace_hash || item?.workspaceHash, item?.session_id || item?.sessionId || item?.id);
  if (!key) return normalizePinnedOrderItems(items);
  return normalizePinnedOrderItems(items).filter((entry) => pinnedOrderKey(entry.workspace_hash, entry.session_id) !== key);
}

export function reorderPinnedSessionId(ids = [], sourceId = '', targetId = '', placement = 'before') {
  const source = String(sourceId || '');
  const target = String(targetId || '');
  const current = normalizePinnedIds(ids);
  if (!source || !target || source === target) return current;
  if (!current.includes(source) || !current.includes(target)) return current;

  const withoutSource = current.filter((id) => id !== source);
  const targetIndex = withoutSource.indexOf(target);
  if (targetIndex < 0) return current;
  const insertIndex = placement === 'after' ? targetIndex + 1 : targetIndex;
  return [
    ...withoutSource.slice(0, insertIndex),
    source,
    ...withoutSource.slice(insertIndex),
  ];
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

export function pinnedSessionsForList(sessions = [], pinnedByWorkspace, pinnedOrderItems = []) {
  const byWorkspaceAndId = new Map();
  for (const session of Array.isArray(sessions) ? sessions : []) {
    const id = session?.id || session?.session_id || '';
    const workspaceHash = session?.workspace_hash || session?.workspaceHash || '';
    if (!id) continue;
    byWorkspaceAndId.set(`${workspaceHash}\u0000${id}`, session);
  }

  const byPinnedKey = new Map();
  const baseOrder = [];
  for (const [workspaceHash, ids] of workspaceEntries(pinnedByWorkspace)) {
    for (const id of normalizePinnedIds(ids)) {
      const key = pinnedOrderKey(workspaceHash, id);
      if (!key || byPinnedKey.has(key)) continue;
      const session = byWorkspaceAndId.get(key);
      if (!session) continue;
      byPinnedKey.set(key, { ...session, pinned: true });
      baseOrder.push(key);
    }
  }

  const out = [];
  const emitted = new Set();
  for (const item of normalizePinnedOrderItems(pinnedOrderItems)) {
    const key = pinnedOrderKey(item.workspace_hash, item.session_id);
    const session = byPinnedKey.get(key);
    if (!session || emitted.has(key)) continue;
    emitted.add(key);
    out.push(session);
  }
  for (const key of baseOrder) {
    if (emitted.has(key)) continue;
    const session = byPinnedKey.get(key);
    if (!session) continue;
    emitted.add(key);
    out.push(session);
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
