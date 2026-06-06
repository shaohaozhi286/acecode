export const CHAT_TAIL_FOLLOW_THRESHOLD_PX = 80;

export const CHAT_TAIL_FOLLOW_STATE = Object.freeze({
  FOLLOWING: 'following',
  REVIEWING: 'reviewing',
});

function finiteNumber(value, fallback = 0) {
  const n = Number(value);
  return Number.isFinite(n) ? n : fallback;
}

function nonNegativeNumber(value, fallback = 0) {
  return Math.max(0, finiteNumber(value, fallback));
}

export function chatScrollMetrics(source = {}) {
  return {
    scrollTop: nonNegativeNumber(source?.scrollTop),
    clientHeight: nonNegativeNumber(source?.clientHeight),
    scrollHeight: nonNegativeNumber(source?.scrollHeight),
  };
}

export function chatTailDistance(metrics = {}) {
  const { scrollTop, clientHeight, scrollHeight } = chatScrollMetrics(metrics);
  return Math.max(0, scrollHeight - scrollTop - clientHeight);
}

export function isChatNearTail(metrics = {}, thresholdPx = CHAT_TAIL_FOLLOW_THRESHOLD_PX) {
  const threshold = nonNegativeNumber(thresholdPx, CHAT_TAIL_FOLLOW_THRESHOLD_PX);
  return chatTailDistance(metrics) <= threshold;
}

export function nextChatTailFollowState(currentState = CHAT_TAIL_FOLLOW_STATE.FOLLOWING, action = {}) {
  const current = currentState === CHAT_TAIL_FOLLOW_STATE.REVIEWING
    ? CHAT_TAIL_FOLLOW_STATE.REVIEWING
    : CHAT_TAIL_FOLLOW_STATE.FOLLOWING;

  switch (action?.type) {
    case 'session_reset':
    case 'new_turn':
      return CHAT_TAIL_FOLLOW_STATE.FOLLOWING;
    case 'scroll':
      return isChatNearTail(action.metrics, action.thresholdPx)
        ? CHAT_TAIL_FOLLOW_STATE.FOLLOWING
        : CHAT_TAIL_FOLLOW_STATE.REVIEWING;
    case 'review_pause':
      return CHAT_TAIL_FOLLOW_STATE.REVIEWING;
    default:
      return current;
  }
}

export function shouldAutoFollowChatTail(state = CHAT_TAIL_FOLLOW_STATE.FOLLOWING) {
  return state !== CHAT_TAIL_FOLLOW_STATE.REVIEWING;
}
