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

// 离开尾部的 scroll 事件里,只有"真实的用户上滚"才应该暂停跟随。流式渲染
// 替换 DOM 时,内容高度瞬时回落会让浏览器 clamp/调整 scrollTop 并触发
// scroll 事件 —— 把它误判成用户滚动会让跟随模式反复开关,消息区上下跳动。
// 判据(参考 assistant-ui useThreadViewportAutoScroll):
//   - userGesture(滚轮 / 按住拖动期间)→ 无条件视为用户意图;
//   - 内容高度与上一次 scroll 事件相同且 scrollTop 减小 → 用户上滚;
//   - 其余(高度变化伴随的位移、程序滚动)→ 保持现态。
export function isUserScrollAway(action = {}) {
  if (action?.userGesture) return true;
  const prev = action?.prevMetrics;
  if (!prev) return false;
  const prevMetrics = chatScrollMetrics(prev);
  const metrics = chatScrollMetrics(action?.metrics);
  return metrics.scrollHeight === prevMetrics.scrollHeight
    && metrics.scrollTop < prevMetrics.scrollTop;
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
      if (isChatNearTail(action.metrics, action.thresholdPx)) {
        return CHAT_TAIL_FOLLOW_STATE.FOLLOWING;
      }
      return isUserScrollAway(action) ? CHAT_TAIL_FOLLOW_STATE.REVIEWING : current;
    case 'review_pause':
      return CHAT_TAIL_FOLLOW_STATE.REVIEWING;
    default:
      return current;
  }
}

export function shouldAutoFollowChatTail(state = CHAT_TAIL_FOLLOW_STATE.FOLLOWING) {
  return state !== CHAT_TAIL_FOLLOW_STATE.REVIEWING;
}
