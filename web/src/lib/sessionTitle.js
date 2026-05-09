export function titleFromMessages(messages = []) {
  for (let i = messages.length - 1; i >= 0; --i) {
    const m = messages[i] || {};
    if (m.role === 'user' && typeof m.content === 'string' && m.content.trim()) {
      return m.content.trim();
    }
  }
  return '';
}

export function isUntitledNewSession(session) {
  const s = session || {};
  const explicit = typeof s.title === 'string' ? s.title.trim() : '';
  if (explicit) return false;

  const summary = typeof s.summary === 'string' ? s.summary.trim() : '';
  if (summary) return false;

  const count = Number(s.message_count ?? s.messageCount ?? 0) || 0;
  return count <= 0;
}

export function newSessionDisplayTitle(index) {
  const n = Math.max(1, Number(index) || 1);
  return `新会话${n}`;
}

function sessionWorkspaceKey(session) {
  const s = session || {};
  return String(s.workspace_hash || s.workspaceHash || '');
}

function sessionCreatedTime(session) {
  const s = session || {};
  const created = Date.parse(s.created_at || '');
  if (Number.isFinite(created)) return created;
  const updated = Date.parse(s.updated_at || '');
  return Number.isFinite(updated) ? updated : Number.MAX_SAFE_INTEGER;
}

export function withNewSessionDisplayTitles(sessions = []) {
  if (!Array.isArray(sessions) || sessions.length === 0) return [];

  const untitled = [];
  sessions.forEach((session, index) => {
    if (!isUntitledNewSession(session)) return;
    untitled.push({
      index,
      workspace: sessionWorkspaceKey(session),
      created: sessionCreatedTime(session),
    });
  });

  untitled.sort((a, b) => {
    if (a.workspace !== b.workspace) return a.workspace < b.workspace ? -1 : 1;
    if (a.created !== b.created) return a.created - b.created;
    return a.index - b.index;
  });

  const counts = new Map();
  const labels = new Map();
  for (const item of untitled) {
    const next = (counts.get(item.workspace) || 0) + 1;
    counts.set(item.workspace, next);
    labels.set(item.index, newSessionDisplayTitle(next));
  }

  return sessions.map((session, index) => (
    labels.has(index)
      ? { ...session, displayTitle: labels.get(index) }
      : session
  ));
}

export function sessionDisplayTitle(session, fallback = '') {
  const s = session || {};
  const explicit = typeof s.title === 'string' ? s.title.trim() : '';
  if (explicit) return explicit;

  const summary = typeof s.summary === 'string' ? s.summary.trim() : '';
  if (summary) return summary;

  const count = Number(s.message_count ?? s.messageCount ?? 0) || 0;
  if (count > 0) {
    const when = s.updated_at || s.created_at || '';
    return when ? `${when}  ${count} msgs` : `${count} msgs`;
  }

  const display = typeof s.displayTitle === 'string' ? s.displayTitle.trim()
    : (typeof s.display_title === 'string' ? s.display_title.trim() : '');
  if (display) return display;

  const id = String(s.sessionId || s.session_id || s.id || '').trim();
  const fallbackText = typeof fallback === 'string' ? fallback.trim() : '';
  if (fallbackText && fallbackText !== id) return fallbackText;

  return newSessionDisplayTitle(1);
}
