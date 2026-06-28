import { projectCollapsedTranscriptItems } from './transcriptProjection.js';

const DEFAULT_DELAY_MS = 180;
const DEFAULT_RETRY_DELAY_MS = 450;
const DEFAULT_MAX_RETRIES = 1;

function stringValue(value) {
  return String(value ?? '').trim();
}

function isUserMessage(item) {
  return item?.kind === 'msg' && item.role === 'user';
}

function isAssistantMessage(item) {
  return item?.kind === 'msg' && item.role === 'assistant';
}

function stableJsonValue(value) {
  if (value == null) return null;
  if (Array.isArray(value)) return value.map((item) => stableJsonValue(item));
  if (typeof value === 'object') {
    const out = {};
    for (const key of Object.keys(value).sort()) {
      if (key === 'id' || key === 'ts' || key === 'timestamp' || key === 'expanded') continue;
      out[key] = stableJsonValue(value[key]);
    }
    return out;
  }
  if (typeof value === 'number') return Number.isFinite(value) ? value : null;
  if (typeof value === 'boolean' || typeof value === 'string') return value;
  return String(value);
}

function normalizedToolSignature(tool = {}) {
  return {
    isTaskComplete: tool.isTaskComplete === true,
    isDone: tool.isDone === true,
    success: tool.success === true ? true : (tool.success === false ? false : null),
    tool: stringValue(tool.tool),
    toolCallId: stringValue(tool.toolCallId ?? tool.tool_call_id),
    toolIndex: tool.toolIndex ?? tool.tool_index ?? null,
    displayOverride: stringValue(tool.displayOverride),
    title: stringValue(tool.title),
    summary: stableJsonValue(tool.summary),
    output: String(tool.output ?? ''),
    hunks: stableJsonValue(tool.hunks),
    attachments: stableJsonValue(tool.attachments),
    askUserQuestionResult: stableJsonValue(tool.askUserQuestionResult),
  };
}

function normalizedProjectedItem(item) {
  if (!item || typeof item !== 'object') return null;
  const base = {
    kind: stringValue(item.kind),
    role: stringValue(item.role),
    mode: stringValue(item.mode),
    content: String(item.content ?? ''),
    title: stringValue(item.title),
    summary: String(item.summary ?? ''),
    source: stringValue(item.source),
    toolCallId: stringValue(item.toolCallId ?? item.tool_call_id),
    toolIndex: item.toolIndex ?? item.tool_index ?? null,
    contentParts: stableJsonValue(item.contentParts),
    metadata: stableJsonValue(item.metadata),
  };
  if (item.kind === 'tool') {
    base.tool = normalizedToolSignature(item.tool || {});
  }
  if (Array.isArray(item.collapsedItems)) {
    base.collapsedItems = item.collapsedItems.map((child) => normalizedProjectedItem(child));
  }
  if (Array.isArray(item.detailItems)) {
    base.detailItems = item.detailItems.map((child) => normalizedProjectedItem(child));
  }
  if (item.sourceItem) {
    base.sourceItem = normalizedProjectedItem(item.sourceItem);
  }
  return base;
}

function cloneRawItem(item) {
  if (typeof structuredClone === 'function') {
    try {
      return structuredClone(item);
    } catch {
      // Fall through to JSON clone for plain transcript items.
    }
  }
  return JSON.parse(JSON.stringify(item));
}

function withFreshIds(items, startId) {
  let nextId = Math.max(1, Number(startId) || 1);
  const cloned = (Array.isArray(items) ? items : []).map((item) => {
    const copy = cloneRawItem(item);
    copy.id = nextId;
    nextId += 1;
    return copy;
  });
  return { items: cloned, nextItemId: nextId };
}

export function completedTurnSelfHealEnabled(health = {}) {
  if (!health || typeof health !== 'object') return false;
  const raw = health?.features?.completed_turn_self_heal
    ?? health?.features?.completedTurnSelfHeal
    ?? health?.completed_turn_self_heal;
  if (raw === true) return true;
  if (raw === false) return false;
  if (raw && typeof raw === 'object') return raw.enabled !== false;
  return true;
}

export function findLatestUserTurnAnchor(items = []) {
  const source = Array.isArray(items) ? items : [];
  let userOrdinal = 0;
  let found = null;
  for (let index = 0; index < source.length; index += 1) {
    const item = source[index];
    if (!isUserMessage(item)) continue;
    userOrdinal += 1;
    const messageId = stringValue(item.messageId);
    const content = String(item.content ?? '');
    found = {
      index,
      userOrdinal,
      messageId,
      content,
      key: messageId
        ? `user-id:${messageId}`
        : `user-ordinal:${userOrdinal}:content:${content}`,
    };
  }
  return found;
}

export function extractLatestAiResponseFragment(items = []) {
  const source = Array.isArray(items) ? items : [];
  const anchor = findLatestUserTurnAnchor(source);
  if (!anchor) return null;
  return {
    anchor,
    anchorItem: source[anchor.index],
    items: source.slice(anchor.index + 1),
  };
}

export function latestTurnProjectedSignature(items = []) {
  const fragment = extractLatestAiResponseFragment(items);
  if (!fragment) return null;
  const projected = projectCollapsedTranscriptItems(
    [fragment.anchorItem, ...fragment.items],
    { deferTrailingToolSummary: false },
  );
  const tail = projected.slice(1).map((item) => normalizedProjectedItem(item));
  return JSON.stringify(tail);
}

export function latestTurnHasAssistantText(items = []) {
  const fragment = extractLatestAiResponseFragment(items);
  if (!fragment) return false;
  return fragment.items.some((item) => (
    isAssistantMessage(item) && String(item.content || '').trim().length > 0
  ));
}

export function replaceLatestTurnAfterAnchor(currentState, canonicalState) {
  const currentItems = Array.isArray(currentState?.items) ? currentState.items : [];
  const canonicalItems = Array.isArray(canonicalState?.items) ? canonicalState.items : [];
  const currentFragment = extractLatestAiResponseFragment(currentItems);
  const canonicalFragment = extractLatestAiResponseFragment(canonicalItems);
  if (!currentFragment || !canonicalFragment) {
    return { replaced: false, reason: 'missing_anchor', state: currentState };
  }
  if (currentFragment.anchor.key !== canonicalFragment.anchor.key) {
    return { replaced: false, reason: 'anchor_mismatch', state: currentState };
  }

  const prefix = currentItems.slice(0, currentFragment.anchor.index + 1);
  const { items: replacementItems, nextItemId } = withFreshIds(
    canonicalFragment.items,
    currentState?.nextItemId,
  );
  const nextState = {
    ...currentState,
    items: [...prefix, ...replacementItems],
    nextItemId,
    streamingId: null,
    toolMap: new Map(),
    activity: null,
  };
  return { replaced: true, reason: 'replaced', state: nextState };
}

export function reconcileLatestCompletedTurn(currentState, canonicalState, snapshot = null) {
  const currentItems = Array.isArray(currentState?.items) ? currentState.items : [];
  const canonicalItems = Array.isArray(canonicalState?.items) ? canonicalState.items : [];
  const currentFragment = extractLatestAiResponseFragment(currentItems);
  const canonicalFragment = extractLatestAiResponseFragment(canonicalItems);
  if (!currentFragment || !canonicalFragment) {
    return { replaced: false, reason: 'missing_anchor', state: currentState };
  }
  if (snapshot?.anchorKey && currentFragment.anchor.key !== snapshot.anchorKey) {
    return { replaced: false, reason: 'stale_anchor', state: currentState };
  }
  if (currentFragment.anchor.key !== canonicalFragment.anchor.key) {
    return { replaced: false, reason: 'anchor_mismatch', state: currentState };
  }
  if (canonicalFragment.items.length === 0 && currentFragment.items.length > 0) {
    return { replaced: false, reason: 'canonical_incomplete', state: currentState };
  }
  if (latestTurnHasAssistantText(currentItems) && !latestTurnHasAssistantText(canonicalItems)) {
    return { replaced: false, reason: 'canonical_incomplete', state: currentState };
  }

  const currentSignature = latestTurnProjectedSignature(currentItems);
  const canonicalSignature = latestTurnProjectedSignature(canonicalItems);
  if (currentSignature === canonicalSignature) {
    return { replaced: false, reason: 'signature_match', state: currentState };
  }
  return replaceLatestTurnAfterAnchor(currentState, canonicalState);
}

export function latestTurnSelfHealSnapshot({ sessionId = '', state = null } = {}) {
  const sid = stringValue(sessionId);
  if (!sid) return null;
  const anchor = findLatestUserTurnAnchor(state?.items);
  if (!anchor) return null;
  return { sessionId: sid, anchorKey: anchor.key };
}

export function isLatestTurnSelfHealSnapshotCurrent(snapshot, { sessionId = '', state = null } = {}) {
  if (!snapshot) return false;
  if (stringValue(sessionId) !== stringValue(snapshot.sessionId)) return false;
  const anchor = findLatestUserTurnAnchor(state?.items);
  return !!anchor && anchor.key === snapshot.anchorKey;
}

export function shouldScheduleCompletedTurnSelfHeal({
  enabled = false,
  sessionId = '',
  isLive = false,
  state = null,
  lastScheduledKey = '',
} = {}) {
  if (!enabled) return { shouldSchedule: false, reason: 'disabled' };
  const sid = stringValue(sessionId);
  if (!sid) return { shouldSchedule: false, reason: 'missing_session' };
  if (!isLive) return { shouldSchedule: false, reason: 'not_live' };
  const snapshot = latestTurnSelfHealSnapshot({ sessionId: sid, state });
  if (!snapshot) return { shouldSchedule: false, reason: 'missing_anchor' };
  const key = `${snapshot.sessionId}:${snapshot.anchorKey}`;
  if (key && key === lastScheduledKey) {
    return { shouldSchedule: false, reason: 'duplicate_turn', key, snapshot };
  }
  return { shouldSchedule: true, reason: 'scheduled', key, snapshot };
}

export function createCompletedTurnSelfHealScheduler(options = {}) {
  const setTimer = options.setTimer || ((fn, ms) => window.setTimeout(fn, ms));
  const clearTimer = options.clearTimer || ((id) => window.clearTimeout(id));
  const delayMs = Number.isFinite(options.delayMs) ? options.delayMs : DEFAULT_DELAY_MS;
  const retryDelayMs = Number.isFinite(options.retryDelayMs) ? options.retryDelayMs : DEFAULT_RETRY_DELAY_MS;
  const maxRetries = Math.max(0, Number.isFinite(options.maxRetries) ? options.maxRetries : DEFAULT_MAX_RETRIES);

  let timer = 0;
  let generation = 0;
  let lastScheduledKey = '';

  const clearPendingTimer = () => {
    if (timer) {
      clearTimer(timer);
      timer = 0;
    }
  };

  const currentSessionId = () => stringValue(options.getSessionId?.());
  const currentState = () => options.getState?.() || null;
  const stillCurrent = (request) => (
    request.generation === generation &&
    options.isVisible?.() !== false &&
    isLatestTurnSelfHealSnapshotCurrent(request.snapshot, {
      sessionId: currentSessionId(),
      state: currentState(),
    })
  );

  const run = async (request, attempt = 0) => {
    if (!stillCurrent(request)) return;
    try {
      const data = await options.fetchCanonicalHistory?.(request.sessionId);
      if (!stillCurrent(request)) return;
      const result = options.applyCanonicalHistory?.(data, request.snapshot) || {};
      if (result.reason === 'canonical_incomplete' && attempt < maxRetries) {
        timer = setTimer(() => {
          timer = 0;
          void run(request, attempt + 1);
        }, retryDelayMs);
      }
    } catch (error) {
      options.onError?.(error);
    }
  };

  return {
    schedule() {
      if (options.isVisible?.() === false) {
        return { scheduled: false, reason: 'not_visible' };
      }
      const decision = shouldScheduleCompletedTurnSelfHeal({
        enabled: options.getEnabled?.() === true,
        sessionId: currentSessionId(),
        isLive: options.getIsLive?.() === true,
        state: currentState(),
        lastScheduledKey,
      });
      if (!decision.shouldSchedule) return { scheduled: false, reason: decision.reason };

      lastScheduledKey = decision.key;
      generation += 1;
      const request = {
        generation,
        sessionId: decision.snapshot.sessionId,
        snapshot: decision.snapshot,
      };
      clearPendingTimer();
      timer = setTimer(() => {
        timer = 0;
        void run(request, 0);
      }, delayMs);
      return { scheduled: true, key: decision.key };
    },
    cancel() {
      generation += 1;
      clearPendingTimer();
    },
    getLastScheduledKey() {
      return lastScheduledKey;
    },
  };
}
