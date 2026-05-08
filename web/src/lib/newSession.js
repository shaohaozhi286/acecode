import { withNewSessionDisplayTitles } from './sessionTitle.js';

function pickWorkspaceHash(source) {
  if (!source || typeof source !== 'object') return '';
  return source.workspaceHash || source.workspace_hash || source.hash || '';
}

function isRealWorkspaceHash(hash) {
  return !!hash && hash !== '__local__';
}

export function sessionRefFromCreateResponse(response, fallbackRef = {}, health = null) {
  const r = response && typeof response === 'object' ? response : {};
  const fallback = fallbackRef && typeof fallbackRef === 'object' ? fallbackRef : {};
  const sessionId = r.session_id || r.id || '';
  if (!sessionId) {
    throw new Error('create session returned no session_id');
  }

  const workspaceHash = r.workspace_hash || pickWorkspaceHash(fallback);
  const next = {
    sessionId,
    contextId: r.context_id || fallback.contextId || 'default',
    cwd: r.cwd || fallback.cwd || health?.cwd || '',
  };
  if (workspaceHash) next.workspaceHash = workspaceHash;
  if (r.title || fallback.title) next.title = r.title || fallback.title;
  if (r.summary || fallback.summary) next.summary = r.summary || fallback.summary;
  if (r.message_count != null) next.message_count = r.message_count;
  else if (fallback.message_count != null) next.message_count = fallback.message_count;
  if (r.created_at || fallback.created_at) next.created_at = r.created_at || fallback.created_at;
  if (r.updated_at || fallback.updated_at) next.updated_at = r.updated_at || fallback.updated_at;

  for (const key of ['workspaceName', 'port', 'token', 'context_window', 'model', 'model_name', 'model_preset', 'provider']) {
    if (r[key] != null) next[key] = r[key];
    else if (fallback[key] != null) next[key] = fallback[key];
  }

  return next;
}

async function resolveDisplayTitle(apiClient, sessionRef) {
  const sessionId = sessionRef?.sessionId || sessionRef?.id || '';
  if (!sessionId) return '';

  const workspaceHash = pickWorkspaceHash(sessionRef);
  const list = isRealWorkspaceHash(workspaceHash)
    ? (typeof apiClient.listWorkspaceSessions === 'function'
        ? await apiClient.listWorkspaceSessions(workspaceHash)
        : [])
    : (typeof apiClient.listSessions === 'function'
        ? await apiClient.listSessions()
        : []);
  const sessions = Array.isArray(list) ? [...list] : [];
  if (!sessions.some((s) => (s?.id || s?.session_id || s?.sessionId) === sessionId)) {
    sessions.push({
      ...sessionRef,
      id: sessionId,
      workspace_hash: workspaceHash,
    });
  }
  const decorated = withNewSessionDisplayTitles(sessions);
  const found = decorated.find((s) => (s?.id || s?.session_id || s?.sessionId) === sessionId);
  return found?.displayTitle || found?.display_title || '';
}

export async function createNewSessionForActiveWorkspace(apiClient, activeRef = {}, health = null) {
  if (!apiClient || typeof apiClient.createSession !== 'function') {
    throw new Error('api client unavailable');
  }

  const workspaceHash = pickWorkspaceHash(activeRef);
  const response = isRealWorkspaceHash(workspaceHash)
    ? await apiClient.createWorkspaceSession(workspaceHash, {})
    : await apiClient.createSession({});
  const ref = sessionRefFromCreateResponse(response, activeRef, health);
  try {
    const displayTitle = await resolveDisplayTitle(apiClient, ref);
    if (displayTitle) ref.displayTitle = displayTitle;
  } catch {
    // The session was already created; failing to refresh the friendly title
    // should not block switching to it.
  }
  return ref;
}
