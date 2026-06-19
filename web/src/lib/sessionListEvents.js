export const SESSION_LIST_CHANGED_EVENT = 'acecode:session-list-changed';

export function normalizeSessionListChangedDetail(detail = {}) {
  const source = detail && typeof detail === 'object' ? detail : {};
  const session = source.session && typeof source.session === 'object' ? source.session : null;
  const sessionId = String(source.sessionId || source.session_id || session?.id || session?.session_id || '').trim();
  const noWorkspace = !!(
    source.noWorkspace || source.no_workspace || session?.noWorkspace || session?.no_workspace
  );
  const workspaceHash = noWorkspace
    ? ''
    : String(
        source.workspaceHash || source.workspace_hash || session?.workspace_hash || session?.workspaceHash || '',
      ).trim();
  return {
    sessionId,
    workspaceHash,
    noWorkspace,
    reason: String(source.reason || '').trim(),
    session,
  };
}

function customEvent(name, detail) {
  if (typeof CustomEvent === 'function') return new CustomEvent(name, { detail });
  const event = new Event(name);
  Object.defineProperty(event, 'detail', {
    value: detail,
    enumerable: true,
  });
  return event;
}

export function notifySessionListChanged(detail = {}, target = globalThis.window) {
  const normalized = normalizeSessionListChangedDetail(detail);
  if (!target || typeof target.dispatchEvent !== 'function') return normalized;
  target.dispatchEvent(customEvent(SESSION_LIST_CHANGED_EVENT, normalized));
  return normalized;
}
