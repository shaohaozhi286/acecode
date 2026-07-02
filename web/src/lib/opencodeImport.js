function numeric(value, fallback = 0) {
  return Number.isFinite(Number(value)) ? Number(value) : fallback;
}

export function normalizeOpencodeImportSession(session = {}) {
  const id = String(session?.id || session?.opencode_session_id || '').trim();
  const title = String(session?.title || id || '未命名会话');
  const archived = !!session?.archived || numeric(session?.time_archived_ms, 0) > 0;
  return {
    ...session,
    id,
    title,
    archived,
    model: String(session?.model || ''),
    provider: String(session?.provider || ''),
    message_count: Math.max(0, numeric(session?.message_count, 0)),
    part_count: Math.max(0, numeric(session?.part_count, 0)),
    time_updated_ms: Math.max(0, numeric(session?.time_updated_ms, 0)),
  };
}

export function normalizeOpencodeImportPreview(preview = {}) {
  const sessions = Array.isArray(preview?.sessions)
    ? preview.sessions.map(normalizeOpencodeImportSession).filter((session) => session.id)
    : [];
  const count = Number.isFinite(Number(preview?.count))
    ? Math.max(0, Number(preview.count))
    : sessions.length;
  return {
    ...preview,
    sessions,
    count,
    available: !!preview?.available && count > 0,
  };
}

export function defaultOpencodeImportSelection(sessions = []) {
  return sessions
    .filter((session) => !session.archived)
    .map((session) => session.id);
}

export function toggleAllOpencodeImportSelection(sessions = [], selectedIds = []) {
  const ids = sessions.map((session) => session.id).filter(Boolean);
  const selected = new Set(selectedIds);
  const allSelected = ids.length > 0 && ids.every((id) => selected.has(id));
  return allSelected ? [] : ids;
}

export function opencodeImportConfirmationText(count) {
  const n = Number.isFinite(Number(count)) ? Math.max(0, Number(count)) : 0;
  return `即将导入 ${n} 个会话，请确认`;
}

export function opencodeImportProgress(status = {}) {
  const total = Number.isFinite(Number(status?.total)) ? Math.max(0, Number(status.total)) : 0;
  const imported = Number.isFinite(Number(status?.imported)) ? Math.max(0, Number(status.imported)) : 0;
  const failed = Number.isFinite(Number(status?.failed)) ? Math.max(0, Number(status.failed)) : 0;
  const skipped = Number.isFinite(Number(status?.skipped)) ? Math.max(0, Number(status.skipped)) : 0;
  const processed = Math.min(total || imported + failed + skipped, imported + failed + skipped);
  const percent = total > 0 ? Math.min(100, Math.round((processed / total) * 100)) : 0;
  return { total, imported, failed, skipped, processed, percent };
}
