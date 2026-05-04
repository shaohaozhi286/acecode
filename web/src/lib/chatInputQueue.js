export const QUEUED_INPUT_STATE = Object.freeze({
  QUEUED: 'queued',
  SENDING: 'sending',
  FAILED: 'failed',
  COMPLETED: 'completed',
  CANCELLED: 'cancelled',
});

function normalizeSessionId(sessionId) {
  return String(sessionId || '');
}

function normalizeText(text) {
  return String(text ?? '');
}

function cloneItems(state) {
  return Array.isArray(state?.items) ? state.items : [];
}

function nextSequence(state) {
  const next = Number(state?.nextLocalId || 1);
  return Number.isFinite(next) && next > 0 ? next : 1;
}

function buildQueuedId(sessionId, sequence) {
  const sid = normalizeSessionId(sessionId).replace(/[^a-zA-Z0-9_-]+/g, '-');
  return `queued-${sid || 'session'}-${sequence}`;
}

export function createChatInputQueueState(overrides = {}) {
  return {
    nextLocalId: nextSequence(overrides),
    items: cloneItems(overrides),
  };
}

export function enqueueQueuedInput(state, { sessionId, text, now = Date.now() } = {}) {
  const sid = normalizeSessionId(sessionId);
  const content = normalizeText(text);
  if (!sid || content.trim().length === 0) return state || createChatInputQueueState();

  const current = createChatInputQueueState(state);
  const sequence = nextSequence(current);
  const id = buildQueuedId(sid, sequence);
  const item = {
    kind: 'msg',
    id,
    messageId: '',
    role: 'user',
    content,
    ts: now,
    queued: {
      id,
      sessionId: sid,
      state: QUEUED_INPUT_STATE.QUEUED,
      createdAt: now,
      updatedAt: now,
      error: '',
    },
  };

  return {
    ...current,
    nextLocalId: sequence + 1,
    items: [...current.items, item],
  };
}

function updateQueuedInput(state, id, updater) {
  const current = createChatInputQueueState(state);
  let changed = false;
  const items = current.items.map((item) => {
    if (item?.queued?.id !== id) return item;
    const nextItem = updater(item);
    if (nextItem !== item) changed = true;
    return nextItem;
  });
  return changed ? { ...current, items } : current;
}

function setQueuedInputState(state, id, nextState, extraQueued = {}) {
  const now = Date.now();
  return updateQueuedInput(state, id, (item) => ({
    ...item,
    queued: {
      ...item.queued,
      state: nextState,
      updatedAt: now,
      ...extraQueued,
    },
  }));
}

export function cancelQueuedInput(state, id) {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.CANCELLED);
}

export function markQueuedInputSending(state, id, { now = Date.now() } = {}) {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.SENDING, {
    sentAt: now,
    updatedAt: now,
    error: '',
  });
}

export function markQueuedInputFailed(state, id, error = '') {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.FAILED, {
    error: String(error || '发送失败'),
  });
}

export function markQueuedInputCompleted(state, id) {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.COMPLETED, { error: '' });
}

export function retryQueuedInput(state, id) {
  return setQueuedInputState(state, id, QUEUED_INPUT_STATE.QUEUED, { error: '' });
}

export function queuedInputsForSession(state, sessionId, { includeDone = false } = {}) {
  const sid = normalizeSessionId(sessionId);
  const doneStates = new Set([
    QUEUED_INPUT_STATE.COMPLETED,
    QUEUED_INPUT_STATE.CANCELLED,
  ]);
  return cloneItems(state).filter((item) => {
    if (item?.queued?.sessionId !== sid) return false;
    if (includeDone) return true;
    return !doneStates.has(item.queued.state);
  });
}

export function hasSendingQueuedInput(state, sessionId) {
  return queuedInputsForSession(state, sessionId, { includeDone: true })
    .some((item) => item.queued?.state === QUEUED_INPUT_STATE.SENDING);
}

export function nextQueuedInput(state, sessionId) {
  if (hasSendingQueuedInput(state, sessionId)) return null;
  return queuedInputsForSession(state, sessionId, { includeDone: true })
    .find((item) => item.queued?.state === QUEUED_INPUT_STATE.QUEUED) || null;
}

export function completeQueuedInputForMessage(state, { sessionId, content, ts } = {}) {
  const sid = normalizeSessionId(sessionId);
  const text = normalizeText(content);
  const current = createChatInputQueueState(state);
  const matched = current.items.find((item) => {
    if (item?.queued?.sessionId !== sid) return false;
    if (item.queued.state !== QUEUED_INPUT_STATE.SENDING) return false;
    if (normalizeText(item.content) !== text) return false;
    const sentAt = Number(item.queued.sentAt || 0);
    const messageTs = Number(ts || 0);
    return !messageTs || !sentAt || messageTs >= sentAt - 2000;
  });
  return matched ? markQueuedInputCompleted(current, matched.queued.id) : current;
}

export function buildQueuedMessageItems(state, sessionId) {
  return queuedInputsForSession(state, sessionId).map((item) => ({ ...item }));
}
