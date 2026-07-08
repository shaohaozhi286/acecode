function text(value) {
  return String(value == null ? '' : value).trim();
}

function firstText(...values) {
  for (const value of values) {
    const next = text(value);
    if (next) return next;
  }
  return '';
}

function firstDefined(...values) {
  for (const value of values) {
    if (value !== undefined && value !== null) return value;
  }
  return undefined;
}

function boolParam(value) {
  const s = text(value).toLowerCase();
  return s === '1' || s === 'true' || s === 'yes';
}

function ordinalValue(value) {
  if (value === undefined || value === null || value === '') return null;
  const n = Number(value);
  return Number.isInteger(n) && n >= 0 ? n : null;
}

export function sessionJumpMessageOrdinal(...sources) {
  for (const source of sources || []) {
    if (!source || typeof source !== 'object') continue;
    const direct = ordinalValue(source.messageOrdinal ?? source.message_ordinal);
    if (direct !== null) return direct;
    const match = source.searchMatch || source.search_match;
    if (match && typeof match === 'object') {
      const nested = ordinalValue(match.messageOrdinal ?? match.message_ordinal);
      if (nested !== null) return nested;
    }
  }
  return null;
}

function normalizedSearchMatch(...sources) {
  for (const source of sources || []) {
    if (!source || typeof source !== 'object') continue;
    const raw = source.searchMatch || source.search_match;
    const ordinal = sessionJumpMessageOrdinal(raw || {}, source);
    if (ordinal === null) continue;
    const out = raw && typeof raw === 'object' ? { ...raw } : {};
    if (!out.kind) out.kind = 'user_message';
    out.messageOrdinal = ordinal;
    out.message_ordinal = ordinal;
    return out;
  }
  return null;
}

export function sessionJumpId(target = {}) {
  return firstText(target.sessionId, target.session_id, target.id);
}

export function sessionJumpNoWorkspace(target = {}, fallback = {}) {
  return !!(
    target.noWorkspace || target.no_workspace ||
    fallback.noWorkspace || fallback.no_workspace
  );
}

export function sessionJumpWorkspaceHash(target = {}, fallback = {}) {
  if (sessionJumpNoWorkspace(target, fallback)) return '';
  return firstText(
    target.workspaceHash,
    target.workspace_hash,
    target.hash,
    fallback.workspaceHash,
    fallback.workspace_hash,
    fallback.hash,
  );
}

export function openSessionTargetFromSearch(search = '') {
  const raw = text(search);
  const params = new URLSearchParams(raw.startsWith('?') ? raw.slice(1) : raw);
  const sessionId = firstText(params.get('open'));
  if (!sessionId) return null;
  const noWorkspace = boolParam(params.get('no_workspace')) || boolParam(params.get('noWorkspace'));
  const workspaceHash = noWorkspace ? '' : firstText(
    params.get('workspace'),
    params.get('workspace_hash'),
    params.get('workspaceHash'),
  );
  const target = { sessionId, workspaceHash, noWorkspace };
  const ordinal = sessionJumpMessageOrdinal({
    message_ordinal: params.get('message_ordinal'),
    messageOrdinal: params.get('messageOrdinal'),
  });
  if (ordinal !== null) {
    target.search_match = { kind: 'user_message', message_ordinal: ordinal, messageOrdinal: ordinal };
  }
  return target;
}

export function stripOpenSessionParams(search = '') {
  const raw = text(search);
  const params = new URLSearchParams(raw.startsWith('?') ? raw.slice(1) : raw);
  for (const key of ['open', 'workspace', 'workspace_hash', 'workspaceHash', 'no_workspace', 'noWorkspace', 'message_ordinal', 'messageOrdinal']) {
    params.delete(key);
  }
  return params.toString();
}

export function desktopOpenSessionUrl({
  port,
  token,
  sessionId,
  workspaceHash = '',
  noWorkspace = false,
  messageOrdinal = null,
  protocol = 'http:',
} = {}) {
  const sid = text(sessionId);
  if (!port || !sid) return '';
  const params = new URLSearchParams();
  if (token != null) params.set('token', String(token));
  params.set('open', sid);
  const ws = text(workspaceHash);
  if (noWorkspace) params.set('no_workspace', '1');
  else if (ws) params.set('workspace', ws);
  const ordinal = ordinalValue(messageOrdinal);
  if (ordinal !== null) params.set('message_ordinal', String(ordinal));
  const scheme = text(protocol).replace(/:$/, '') || 'http';
  return `${scheme}://127.0.0.1:${encodeURIComponent(String(port))}/?${params.toString()}`;
}

export function sessionRefFromJumpTarget(target = {}, resumeResult = {}, fallback = {}) {
  const id = sessionJumpId(resumeResult) || sessionJumpId(target) || sessionJumpId(fallback);
  const noWorkspace = sessionJumpNoWorkspace(resumeResult, target) || sessionJumpNoWorkspace(target, fallback);
  const workspaceHash = noWorkspace ? '' : (
    sessionJumpWorkspaceHash(resumeResult) ||
    sessionJumpWorkspaceHash(target) ||
    sessionJumpWorkspaceHash(fallback)
  );
  const ref = {
    workspaceHash,
    noWorkspace,
    contextId: firstText(resumeResult.context_id, resumeResult.contextId, target.context_id, target.contextId, fallback.contextId) || 'default',
    sessionId: id,
  };

  const cwd = noWorkspace ? '' : firstText(resumeResult.cwd, target.cwd, fallback.cwd);
  if (cwd || noWorkspace) ref.cwd = cwd;
  const searchMatch = normalizedSearchMatch(resumeResult, target, fallback);
  if (searchMatch) ref.searchMatch = searchMatch;
  for (const key of ['port', 'token']) {
    const value = firstDefined(resumeResult[key], target[key], fallback[key]);
    if (value != null) ref[key] = value;
  }
  const copyPairs = [
    ['displayTitle', ['displayTitle', 'display_title']],
    ['title', ['title']],
    ['summary', ['summary']],
    ['provider', ['provider']],
    ['model', ['model']],
    ['model_name', ['model_name']],
    ['model_preset', ['model_preset']],
    ['context_window', ['context_window']],
    ['message_count', ['message_count', 'messageCount']],
    ['created_at', ['created_at', 'createdAt']],
    ['updated_at', ['updated_at', 'updatedAt']],
  ];
  for (const [outKey, keys] of copyPairs) {
    const values = [];
    for (const key of keys) values.push(resumeResult[key], target[key], fallback[key]);
    const value = firstDefined(...values);
    if (value != null && value !== '') ref[outKey] = value;
  }
  const deleted = firstDefined(
    resumeResult.deleted,
    resumeResult.model_deleted,
    resumeResult.modelDeleted,
    target.deleted,
    target.model_deleted,
    target.modelDeleted,
    fallback.deleted,
    fallback.model_deleted,
    fallback.modelDeleted,
  );
  if (deleted != null) ref.deleted = !!deleted;
  return ref;
}
