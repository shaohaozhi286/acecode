export const SIDEBAR_SESSION_COLLAPSE_LIMIT = 5;

function sessionId(session) {
  return String(session?.id || session?.session_id || session?.sessionId || '').trim();
}

function sessionWorkspace(session) {
  return String(session?.workspace_hash || session?.workspaceHash || '').trim();
}

function isNoWorkspaceSession(session) {
  return !!(session?.noWorkspace || session?.no_workspace);
}

function sessionKey(session) {
  const id = sessionId(session);
  return id ? `${sessionWorkspace(session)}\u0000${id}` : '';
}

function sessionTime(session) {
  const s = session || {};
  const updated = Date.parse(s.updated_at || s.updatedAt || '');
  if (Number.isFinite(updated)) return updated;
  const created = Date.parse(s.created_at || s.createdAt || '');
  if (Number.isFinite(created)) return created;
  return 0;
}

function numericCounter(value) {
  const n = Number(value);
  return Number.isFinite(n) && n >= 0 ? n : null;
}

function contentCounter(session, snakeName, camelName) {
  const s = session || {};
  return numericCounter(s[snakeName] ?? s[camelName]);
}

function sessionContentChanged(previous, next) {
  const prevMessages = contentCounter(previous, 'message_count', 'messageCount');
  const nextMessages = contentCounter(next, 'message_count', 'messageCount');
  if (prevMessages != null && nextMessages != null && prevMessages !== nextMessages) return true;

  const prevTurns = contentCounter(previous, 'turn_count', 'turnCount');
  const nextTurns = contentCounter(next, 'turn_count', 'turnCount');
  if (prevTurns != null && nextTurns != null && prevTurns !== nextTurns) return true;

  return false;
}

export function sidebarSessionHasWorktree(session = {}) {
  const worktree = session?.worktree;
  if (!worktree || typeof worktree !== 'object') return false;
  return [worktree.name, worktree.branch]
    .some((value) => String(value || '').trim().length > 0);
}

export function sidebarSessionMarker(session = {}) {
  const loopExecution = session?.loop_execution || session?.loopExecution;
  if (loopExecution && typeof loopExecution === 'object') {
    const hasLoopOrigin = [loopExecution.loop_id, loopExecution.loopId, loopExecution.run_id, loopExecution.runId]
      .some((value) => String(value || '').trim().length > 0);
    if (hasLoopOrigin) return 'loop';
  }
  return sidebarSessionHasWorktree(session) ? 'worktree' : '';
}

export function expandedSessionListsAfterWorkspaceCollapseAll(
  currentExpanded = new Set(),
  workspaces = [],
) {
  const next = currentExpanded instanceof Set
    ? new Set(currentExpanded)
    : new Set();
  const list = Array.isArray(workspaces) ? workspaces : [];
  list.forEach((workspace) => {
    const hash = typeof workspace === 'string'
      ? workspace.trim()
      : String(
        workspace?.hash
        || workspace?.workspace_hash
        || workspace?.workspaceHash
        || '',
      ).trim();
    if (hash) next.delete(hash);
  });
  return next;
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

export function sidebarRevealTarget(activeRef = {}) {
  const id = sessionId(activeRef);
  if (!id) return { sessionId: '', workspaceHash: '', noWorkspace: false };
  const noWorkspace = isNoWorkspaceSession(activeRef);
  return {
    sessionId: id,
    workspaceHash: noWorkspace ? '' : sessionWorkspace(activeRef),
    noWorkspace,
  };
}

export function sessionMatchesRevealTarget(session = {}, target = {}) {
  const targetId = String(target?.sessionId || target?.id || target?.session_id || '').trim();
  if (!targetId || sessionId(session) !== targetId) return false;
  const targetNoWorkspace = !!(target?.noWorkspace || target?.no_workspace);
  if (targetNoWorkspace) return isNoWorkspaceSession(session) || !sessionWorkspace(session);
  const targetWorkspace = String(target?.workspaceHash || target?.workspace_hash || '').trim();
  if (!targetWorkspace) return !isNoWorkspaceSession(session);
  return sessionWorkspace(session) === targetWorkspace;
}

export function sessionListNeedsRevealExpansion(sessions = [], target = {}, expanded = false, limit = SIDEBAR_SESSION_COLLAPSE_LIMIT) {
  if (expanded || !target?.sessionId) return false;
  const projection = sidebarSessionProjection(sessions, false, limit);
  if (!projection.collapsible) return false;
  const hasTarget = sessions.some((session) => sessionMatchesRevealTarget(session, target));
  if (!hasTarget) return false;
  return !projection.visibleSessions.some((session) => sessionMatchesRevealTarget(session, target));
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

export function reconcileSidebarSessions(previousSessions = [], incomingSessions = []) {
  const previous = Array.isArray(previousSessions) ? previousSessions : [];
  const incoming = Array.isArray(incomingSessions) ? incomingSessions : [];
  if (previous.length === 0) return sortSidebarSessionsNewestFirst(incoming);

  const previousByKey = new Map();
  for (const session of previous) {
    const key = sessionKey(session);
    if (key && !previousByKey.has(key)) previousByKey.set(key, session);
  }

  const incomingByKey = new Map();
  for (const session of incoming) {
    const key = sessionKey(session);
    if (key) incomingByKey.set(key, session);
  }

  const promoted = new Set();
  const top = [];
  for (const session of sortSidebarSessionsNewestFirst(incoming)) {
    const key = sessionKey(session);
    if (!key || promoted.has(key)) continue;
    const previousSession = previousByKey.get(key);
    if (!previousSession || sessionContentChanged(previousSession, session)) {
      promoted.add(key);
      top.push(session);
    }
  }

  const stable = [];
  const emitted = new Set(promoted);
  for (const session of previous) {
    const key = sessionKey(session);
    if (!key || emitted.has(key)) continue;
    const next = incomingByKey.get(key);
    if (!next) continue;
    emitted.add(key);
    stable.push(next);
  }

  return [...top, ...stable];
}

export function upsertSidebarSession(sessions = [], nextSession = null) {
  const id = sessionId(nextSession);
  if (!id) return Array.isArray(sessions) ? sessions : [];

  const list = Array.isArray(sessions) ? sessions : [];
  const nextKey = sessionKey(nextSession);
  const existingIndex = list.findIndex((session) => sessionKey(session) === nextKey);
  if (existingIndex < 0) return sortSidebarSessionsNewestFirst([...list, { ...nextSession, id }]);

  const existing = list[existingIndex];
  const merged = { ...existing, ...nextSession, id };
  const remaining = list.filter((_, index) => index !== existingIndex);
  if (sessionContentChanged(existing, merged)) return [merged, ...remaining];
  return [
    ...list.slice(0, existingIndex),
    merged,
    ...list.slice(existingIndex + 1),
  ];
}
