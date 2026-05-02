export function titleFromMessages(messages = []) {
  for (let i = messages.length - 1; i >= 0; --i) {
    const m = messages[i] || {};
    if (m.role === 'user' && typeof m.content === 'string' && m.content.trim()) {
      return m.content.trim();
    }
  }
  return '';
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

  return fallback || s.sessionId || s.id || '';
}
