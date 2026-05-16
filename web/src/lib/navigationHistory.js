const MAX_HISTORY = 80;
const SEP = '\u001f';

function refValue(ref, ...keys) {
  for (const key of keys) {
    const value = ref && ref[key];
    if (value != null && value !== '') return String(value);
  }
  return '';
}

export function navigationKey(ref) {
  if (!ref || typeof ref !== 'object') return '';
  const type = ref.home ? 'home' : 'session';
  return [
    type,
    refValue(ref, 'workspaceHash', 'workspace_hash'),
    refValue(ref, 'sessionId', 'session_id', 'id'),
    refValue(ref, 'contextId', 'context_id'),
    refValue(ref, 'cwd'),
  ].join(SEP);
}

export function sameNavigationRef(a, b) {
  return navigationKey(a) === navigationKey(b);
}

export function pushNavigation(history, currentRef, nextRef) {
  const currentKey = navigationKey(currentRef);
  const nextKey = navigationKey(nextRef);
  if (!currentKey || currentKey === nextKey) {
    return normalizeHistory(history);
  }
  const back = [...normalizeHistory(history).back, currentRef].slice(-MAX_HISTORY);
  return { back, forward: [] };
}

export function goBack(history, currentRef) {
  const normalized = normalizeHistory(history);
  if (normalized.back.length === 0) {
    return { history: normalized, activeRef: currentRef };
  }
  const activeRef = normalized.back[normalized.back.length - 1];
  const back = normalized.back.slice(0, -1);
  const forward = navigationKey(currentRef)
    ? [currentRef, ...normalized.forward].slice(0, MAX_HISTORY)
    : normalized.forward;
  return { history: { back, forward }, activeRef };
}

export function goForward(history, currentRef) {
  const normalized = normalizeHistory(history);
  if (normalized.forward.length === 0) {
    return { history: normalized, activeRef: currentRef };
  }
  const activeRef = normalized.forward[0];
  const forward = normalized.forward.slice(1);
  const back = navigationKey(currentRef)
    ? [...normalized.back, currentRef].slice(-MAX_HISTORY)
    : normalized.back;
  return { history: { back, forward }, activeRef };
}

export function normalizeHistory(history) {
  return {
    back: Array.isArray(history?.back) ? history.back.filter(Boolean).slice(-MAX_HISTORY) : [],
    forward: Array.isArray(history?.forward) ? history.forward.filter(Boolean).slice(0, MAX_HISTORY) : [],
  };
}
