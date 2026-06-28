export const NO_FEEDBACK_SESSION_KEY = '';

function sessionIdOf(session) {
  return String(session?.id || session?.session_id || session?.sessionId || '').trim();
}

function workspaceHashOf(session) {
  return String(session?.workspace_hash || session?.workspaceHash || '').trim();
}

export function feedbackSessionKey(session) {
  const id = sessionIdOf(session);
  if (!id) return NO_FEEDBACK_SESSION_KEY;
  return `${encodeURIComponent(workspaceHashOf(session))}:${encodeURIComponent(id)}`;
}

export function normalizeDesktopFeedbackSessions(raw) {
  const list = Array.isArray(raw) ? raw : (Array.isArray(raw?.sessions) ? raw.sessions : []);
  const out = [];
  const seen = new Set();
  for (const item of list) {
    const id = sessionIdOf(item);
    if (!id) continue;
    const normalized = {
      ...item,
      id,
      session_id: id,
      workspace_hash: workspaceHashOf(item),
    };
    const key = feedbackSessionKey(normalized);
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(normalized);
  }
  return out;
}

export function selectedFeedbackSessionFromKey(sessions, key) {
  if (!key) return null;
  return normalizeDesktopFeedbackSessions(sessions).find((session) => feedbackSessionKey(session) === key) || null;
}

export function buildDesktopFeedbackPayload({ feedbackText = '', selectedSession = null } = {}) {
  const payload = { feedback_text: String(feedbackText || '') };
  const id = sessionIdOf(selectedSession);
  if (id) {
    payload.session_id = id;
    payload.workspace_hash = workspaceHashOf(selectedSession);
  }
  return payload;
}
