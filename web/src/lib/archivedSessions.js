export function archivedSessionTarget(item = {}) {
  const id = String(item?.id || item?.session_id || item?.sessionId || '').trim();
  const workspaceHash = String(
    item?.workspace_hash || item?.workspaceHash || '',
  ).trim();
  if (!id) return { id: '', workspaceHash, key: '' };
  const scope = workspaceHash || '__local__';
  return {
    id,
    workspaceHash,
    key: JSON.stringify([scope, id]),
  };
}

export function archivedSessionKey(item) {
  return archivedSessionTarget(item).key;
}

function keySet(keys) {
  if (keys instanceof Set) return keys;
  return new Set(Array.isArray(keys) ? keys : []);
}

export function selectedArchivedSessions(items, selectedKeys) {
  const selected = keySet(selectedKeys);
  return (Array.isArray(items) ? items : []).filter((item) => {
    const key = archivedSessionKey(item);
    return key && selected.has(key);
  });
}

export function removeArchivedSessionsByKey(items, removedKeys) {
  const removed = keySet(removedKeys);
  return (Array.isArray(items) ? items : []).filter((item) => {
    const key = archivedSessionKey(item);
    return !key || !removed.has(key);
  });
}
