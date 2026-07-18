import assert from 'node:assert/strict';
import {
  CHAT_TAIL_FOLLOW_STATE,
  chatScrollMetrics,
  chatTailDistance,
  isChatNearTail,
  isUserScrollAway,
  nextChatTailFollowState,
  observeChatTailContent,
  shouldAutoFollowChatTail,
} from './chatScrollFollow.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('at-tail metrics are near the chat tail', () => {
  assert.equal(chatTailDistance({ scrollTop: 520, clientHeight: 400, scrollHeight: 1000 }), 80);
  assert.equal(isChatNearTail({ scrollTop: 520, clientHeight: 400, scrollHeight: 1000 }, 80), true);
});

run('away-from-tail metrics are not near the chat tail', () => {
  assert.equal(chatTailDistance({ scrollTop: 360, clientHeight: 400, scrollHeight: 1000 }), 240);
  assert.equal(isChatNearTail({ scrollTop: 360, clientHeight: 400, scrollHeight: 1000 }, 80), false);
});

// 场景:内容高度不变(scrollHeight 1000 → 1000)时用户把 scrollTop 从
// 500 滚到 100 —— 真实的用户上滚。期望:暂停跟随,进入 REVIEWING。
run('scrolling away pauses tail follow', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, {
    type: 'scroll',
    metrics: { scrollTop: 100, clientHeight: 400, scrollHeight: 1000 },
    prevMetrics: { scrollTop: 500, clientHeight: 400, scrollHeight: 1000 },
    thresholdPx: 80,
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.REVIEWING);
  assert.equal(shouldAutoFollowChatTail(state), false);
});

// 回归测试(fix 流式出字时消息区上下跳动):流式渲染替换 DOM 时内容高度
// 瞬时回落,浏览器 clamp scrollTop 触发 scroll 事件 —— scrollHeight 与上次
// 不同,不是用户滚动。期望:保持 FOLLOWING,跟随不被打断(打断后下一帧
// 又恢复,正是"上下跳动"的来源)。
run('content-driven scroll displacement keeps tail follow', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, {
    type: 'scroll',
    metrics: { scrollTop: 700, clientHeight: 400, scrollHeight: 1200 },
    prevMetrics: { scrollTop: 1000, clientHeight: 400, scrollHeight: 1500 },
    thresholdPx: 80,
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
});

// 场景:拖动滚动条(指针按住)期间的滚动,即使内容高度同帧在变,也视为
// 用户意图。期望:userGesture 标记使其进入 REVIEWING。
run('pointer-held scroll away pauses tail follow even while content grows', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, {
    type: 'scroll',
    metrics: { scrollTop: 300, clientHeight: 400, scrollHeight: 1200 },
    prevMetrics: { scrollTop: 500, clientHeight: 400, scrollHeight: 1000 },
    userGesture: true,
    thresholdPx: 80,
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.REVIEWING);
});

// 场景:没有上一次指标(会话刚打开的第一次 scroll 事件,多为程序初始化
// 滚动)且无手势。期望:保持现态,不误切 REVIEWING。
run('scroll without prior metrics keeps current state', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, {
    type: 'scroll',
    metrics: { scrollTop: 100, clientHeight: 400, scrollHeight: 1000 },
    thresholdPx: 80,
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
});

// isUserScrollAway 的判定矩阵:高度稳定 + scrollTop 减小才算用户上滚;
// userGesture 无条件为真;缺 prev / 高度变化 / scrollTop 增大都为假。
run('isUserScrollAway classifies scroll causes', () => {
  assert.equal(isUserScrollAway({
    metrics: { scrollTop: 100, clientHeight: 400, scrollHeight: 1000 },
    prevMetrics: { scrollTop: 500, clientHeight: 400, scrollHeight: 1000 },
  }), true);
  assert.equal(isUserScrollAway({ userGesture: true }), true);
  assert.equal(isUserScrollAway({
    metrics: { scrollTop: 100, clientHeight: 400, scrollHeight: 900 },
    prevMetrics: { scrollTop: 500, clientHeight: 400, scrollHeight: 1000 },
  }), false);
  assert.equal(isUserScrollAway({
    metrics: { scrollTop: 600, clientHeight: 400, scrollHeight: 1000 },
    prevMetrics: { scrollTop: 500, clientHeight: 400, scrollHeight: 1000 },
  }), false);
  assert.equal(isUserScrollAway({
    metrics: { scrollTop: 100, clientHeight: 400, scrollHeight: 1000 },
  }), false);
});

run('returning near the tail resumes tail follow', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, {
    type: 'scroll',
    metrics: { scrollTop: 525, clientHeight: 400, scrollHeight: 1000 },
    thresholdPx: 80,
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
  assert.equal(shouldAutoFollowChatTail(state), true);
});

run('explicit review pause wins even if the viewport is near the tail', () => {
  const state = nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, {
    type: 'review_pause',
    metrics: { scrollTop: 530, clientHeight: 400, scrollHeight: 1000 },
  });
  assert.equal(state, CHAT_TAIL_FOLLOW_STATE.REVIEWING);
});

run('session reset and new turn initialize tail follow', () => {
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, { type: 'session_reset' }),
    CHAT_TAIL_FOLLOW_STATE.FOLLOWING,
  );
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, { type: 'new_turn' }),
    CHAT_TAIL_FOLLOW_STATE.FOLLOWING,
  );
});

run('self-heal replacement preserves the current tail-follow mode', () => {
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.REVIEWING, { type: 'self_heal_replace' }),
    CHAT_TAIL_FOLLOW_STATE.REVIEWING,
  );
  assert.equal(
    nextChatTailFollowState(CHAT_TAIL_FOLLOW_STATE.FOLLOWING, { type: 'self_heal_replace' }),
    CHAT_TAIL_FOLLOW_STATE.FOLLOWING,
  );
});

run('scroll metrics normalize invalid values', () => {
  assert.deepEqual(chatScrollMetrics({ scrollTop: -1, clientHeight: 'abc', scrollHeight: 120 }), {
    scrollTop: 0,
    clientHeight: 0,
    scrollHeight: 120,
  });
});

run('content resize observation delivers changes and disconnects deterministically', () => {
  const target = {};
  let observedTarget = null;
  let observerCallback = null;
  let resizeCount = 0;
  let disconnectCount = 0;

  class FakeResizeObserver {
    constructor(callback) {
      observerCallback = callback;
    }

    observe(value) {
      observedTarget = value;
    }

    disconnect() {
      disconnectCount += 1;
    }
  }

  const disconnect = observeChatTailContent(
    target,
    () => { resizeCount += 1; },
    FakeResizeObserver,
  );
  assert.equal(observedTarget, target);

  observerCallback();
  assert.equal(resizeCount, 1);

  disconnect();
  disconnect();
  assert.equal(disconnectCount, 1);

  observerCallback();
  assert.equal(resizeCount, 1);
});

run('content resize observation is a no-op without runtime support', () => {
  let resizeCount = 0;
  const disconnect = observeChatTailContent(
    {},
    () => { resizeCount += 1; },
    null,
  );
  assert.equal(typeof disconnect, 'function');
  disconnect();
  assert.equal(resizeCount, 0);
});
