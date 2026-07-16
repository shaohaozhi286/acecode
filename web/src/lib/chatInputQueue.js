export const QUEUED_INPUT_STATE = Object.freeze({
  QUEUED: 'queued',
  SENDING: 'sending',
  FAILED: 'failed',
  GUIDING: 'guiding',
  COMPLETED: 'completed',
  CANCELLED: 'cancelled',
});

function normalizeSessionId(sessionId) {
  return String(sessionId || '');
}

function normalizeText(text) {
  return String(text ?? '');
}

function normalizePayload({ text, payload } = {}) {
  if (payload && typeof payload === 'object' && !Array.isArray(payload)) {
    return {
      text: normalizeText(payload.text),
      attachments: Array.isArray(payload.attachments) ? payload.attachments : [],
      contexts: Array.isArray(payload.contexts) ? payload.contexts : [],
    };
  }
  return {
    text: normalizeText(text),
    attachments: [],
    contexts: [],
  };
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

export function enqueueQueuedInput(state, { sessionId, text, payload, now = Date.now() } = {}) {
  const sid = normalizeSessionId(sessionId);
  const queuedPayload = normalizePayload({ text, payload });
  const content = queuedPayload.text;
  const hasExtras = queuedPayload.attachments.length > 0 || queuedPayload.contexts.length > 0;
  if (!sid || (content.trim().length === 0 && !hasExtras)) return state || createChatInputQueueState();

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
      payload: queuedPayload,
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

export function beginQueuedGuidance(state, id) {
  return updateQueuedInput(state, id, (item) => {
    const currentState = item?.queued?.state;
    if (currentState !== QUEUED_INPUT_STATE.QUEUED &&
        currentState !== QUEUED_INPUT_STATE.FAILED) return item;
    return {
      ...item,
      queued: {
        ...item.queued,
        state: QUEUED_INPUT_STATE.GUIDING,
        guidancePreviousState: currentState,
        updatedAt: Date.now(),
      },
    };
  });
}

export function finishQueuedGuidance(state, id, { succeeded = false } = {}) {
  if (succeeded) return cancelQueuedInput(state, id);
  return updateQueuedInput(state, id, (item) => {
    if (item?.queued?.state !== QUEUED_INPUT_STATE.GUIDING) return item;
    const previous = item.queued.guidancePreviousState === QUEUED_INPUT_STATE.FAILED
      ? QUEUED_INPUT_STATE.FAILED
      : QUEUED_INPUT_STATE.QUEUED;
    const queued = { ...item.queued, state: previous, updatedAt: Date.now() };
    delete queued.guidancePreviousState;
    return { ...item, queued };
  });
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

export function queuedInputRequestPayload(item) {
  const clientMessageId = normalizeText(item?.queued?.id).trim();
  if (!clientMessageId) return null;
  const payload = normalizePayload({
    text: item?.content,
    payload: item?.queued?.payload,
  });
  return {
    ...payload,
    client_message_id: clientMessageId,
  };
}

export function acceptedQueuedInputEvent(item, { now = Date.now() } = {}) {
  const clientMessageId = normalizeText(item?.queued?.id).trim();
  if (!clientMessageId) return null;
  const payload = normalizePayload({
    text: item?.content,
    payload: item?.queued?.payload,
  });
  const content = payload.text || (payload.attachments.length > 0 ? '附件消息' : '上下文消息');
  return {
    type: 'queued_input_accepted',
    payload: {
      client_message_id: clientMessageId,
      content,
    },
    timestamp_ms: now,
  };
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
  const items = queuedInputsForSession(state, sessionId, { includeDone: true });
  if (items.some((item) => (
    item.queued?.state === QUEUED_INPUT_STATE.SENDING ||
    item.queued?.state === QUEUED_INPUT_STATE.GUIDING
  ))) return null;
  return items.find((item) => item.queued?.state === QUEUED_INPUT_STATE.QUEUED) || null;
}

export function completeQueuedInputForMessage(
  state,
  { sessionId, content, ts, clientMessageId } = {},
) {
  const sid = normalizeSessionId(sessionId);
  const text = normalizeText(content);
  const correlationId = normalizeText(clientMessageId).trim();
  const current = createChatInputQueueState(state);
  const matched = current.items.find((item) => {
    if (item?.queued?.sessionId !== sid) return false;
    if (item.queued.state !== QUEUED_INPUT_STATE.SENDING) return false;
    if (correlationId) return item.queued.id === correlationId;
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
