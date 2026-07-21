import assert from 'node:assert/strict';
import {
  MAX_CONVERSATION_TURN_SCRUBBER_MARKERS,
  RUNNING_ANSWER_PREVIEW,
  activeConversationTurnIndex,
  activatedConversationTurnIndex,
  buildConversationTurnPreviews,
  centeredConversationTurnWindowStart,
  conversationTurnMarkerDisplacement,
  conversationTurnMarkerLayout,
  conversationTurnPageControlTop,
  conversationTurnPreviewTop,
  conversationTurnSteppedWindowStart,
  conversationTurnWheelDirection,
  conversationTurnWindow,
  conversationTurnWindowStartContainingIndex,
  nearestConversationTurnIndex,
  shouldShowConversationTurnScrubber,
} from './conversationTurnScrubber.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function user(id, content, extras = {}) {
  return {
    kind: 'msg',
    id,
    messageId: `user-${id}`,
    role: 'user',
    content,
    ...extras,
  };
}

function assistant(id, content) {
  return {
    kind: 'msg',
    id,
    messageId: `assistant-${id}`,
    role: 'assistant',
    content,
  };
}

run('turn scrubber appears only from the fifth user question', () => {
  const fourTurns = Array.from({ length: 4 }, (_, index) => ({ itemId: String(index) }));
  const fiveTurns = Array.from({ length: 5 }, (_, index) => ({ itemId: String(index) }));
  assert.equal(shouldShowConversationTurnScrubber(fourTurns), false);
  assert.equal(shouldShowConversationTurnScrubber(fiveTurns), true);
});

run('turn window shows at most twenty markers and paginates only beyond the cap', () => {
  assert.equal(MAX_CONVERSATION_TURN_SCRUBBER_MARKERS, 20);
  assert.deepEqual(
    conversationTurnWindow(20, 7),
    {
      start: 0,
      end: 20,
      visibleCount: 20,
      hasPrevious: false,
      hasNext: false,
      paginated: false,
    },
  );
  assert.deepEqual(
    conversationTurnWindow(21, 0),
    {
      start: 0,
      end: 20,
      visibleCount: 20,
      hasPrevious: false,
      hasNext: true,
      paginated: true,
    },
  );
});

run('turn window arrows move exactly one turn and clamp at both boundaries', () => {
  assert.equal(conversationTurnSteppedWindowStart(0, 1, 40), 1);
  assert.equal(conversationTurnSteppedWindowStart(10, -1, 40), 9);
  assert.equal(conversationTurnSteppedWindowStart(0, -1, 40), 0);
  assert.equal(conversationTurnSteppedWindowStart(20, 1, 40), 20);
});

run('vertical wheel direction matches the paging arrows', () => {
  assert.equal(conversationTurnWheelDirection(-120), -1);
  assert.equal(conversationTurnWheelDirection(120), 1);
  assert.equal(conversationTurnWheelDirection(0), 0);
  assert.equal(conversationTurnWheelDirection(8, 16), 0);
  assert.equal(conversationTurnWheelDirection(Number.NaN), 0);
});

run('upper and lower selections recenter near the middle with edge clamping', () => {
  assert.equal(centeredConversationTurnWindowStart(23, 60), 14);
  assert.equal(centeredConversationTurnWindowStart(36, 60), 27);
  assert.equal(centeredConversationTurnWindowStart(1, 60), 0);
  assert.equal(centeredConversationTurnWindowStart(58, 60), 40);
});

run('active containment keeps an in-window turn and recenters an escaped turn', () => {
  assert.equal(conversationTurnWindowStartContainingIndex(20, 39, 60), 20);
  assert.equal(conversationTurnWindowStartContainingIndex(20, 41, 60), 32);
  assert.equal(conversationTurnWindowStartContainingIndex(20, 2, 60), 0);
});

run('turn projection ignores non-user rows and keeps attachment-only questions jumpable', () => {
  const turns = buildConversationTurnPreviews([
    { kind: 'tool', id: 'tool-1', role: 'tool', content: 'ignored' },
    user(1, '', { contentParts: [{ type: 'file', attachment: { name: 'brief.md' } }] }),
    { kind: 'activity_summary', id: 'activity-1', title: 'ignored' },
    assistant(2, '已查看附件。'),
  ]);
  assert.equal(turns.length, 1);
  assert.equal(turns[0].itemId, '1');
  assert.equal(turns[0].question, '附件消息');
  assert.equal(turns[0].answer, '已查看附件。');
});

run('turn projection prefers a completion summary over the latest assistant message', () => {
  const turns = buildConversationTurnPreviews([
    user(1, '帮我修复这个问题'),
    assistant(2, '我正在检查。'),
    assistant(3, '已经找到原因。'),
    {
      kind: 'completion_summary',
      id: 'completion-1',
      title: '总结：修复完成',
      summary: '**已完成**\n\n- 修复滚动问题',
    },
  ]);
  assert.equal(turns[0].question, '帮我修复这个问题');
  assert.equal(turns[0].answer, '**已完成** - 修复滚动问题');
});

run('running fallback is limited to the current unanswered turn', () => {
  const turns = buildConversationTurnPreviews([
    user(1, '第一个问题'),
    user(2, '第二个问题'),
    { ...assistant(3, '还在流式输出'), streaming: true },
  ], { busy: true });
  assert.equal(turns[0].answer, '暂无回答');
  assert.equal(turns[1].answer, RUNNING_ANSWER_PREVIEW);
  assert.equal(turns[1].running, true);
});

run('marker layout keeps equal ordering gaps and compresses into short rails', () => {
  const spacious = conversationTurnMarkerLayout(5, 120);
  assert.deepEqual(
    spacious.map((marker) => marker.centerY),
    [24, 42, 60, 78, 96],
  );

  const dense = conversationTurnMarkerLayout(5, 48);
  assert.deepEqual(
    dense.map((marker) => marker.centerY),
    [12, 18, 24, 30, 36],
  );
  assert.ok(dense.every((marker) => marker.zoneHeight > 0));
});

run('paging arrows stay immediately outside the marker stack endpoints', () => {
  const spacious = conversationTurnMarkerLayout(
    20,
    520,
    { edgePadding: 34 },
  );
  assert.equal(spacious[0].centerY, 89);
  assert.equal(spacious[spacious.length - 1].centerY, 431);
  assert.equal(
    conversationTurnPageControlTop(spacious[0].centerY, -1, 520),
    51,
  );
  assert.equal(
    conversationTurnPageControlTop(
      spacious[spacious.length - 1].centerY,
      1,
      520,
    ),
    449,
  );
  assert.equal(
    conversationTurnPageControlTop(
      spacious[0].centerY,
      -1,
      520,
      { centerOffset: 20 },
    ) - conversationTurnPageControlTop(spacious[0].centerY, -1, 520),
    8,
  );
  assert.equal(
    conversationTurnPageControlTop(
      spacious[spacious.length - 1].centerY,
      1,
      520,
    ) - conversationTurnPageControlTop(
      spacious[spacious.length - 1].centerY,
      1,
      520,
      { centerOffset: 20 },
    ),
    8,
  );

  const compressed = conversationTurnMarkerLayout(
    20,
    84,
    { edgePadding: 42 },
  );
  assert.equal(
    conversationTurnPageControlTop(compressed[0].centerY, -1, 84),
    4,
  );
  assert.equal(
    conversationTurnPageControlTop(
      compressed[compressed.length - 1].centerY,
      1,
      84,
    ),
    60,
  );
});

run('paging arrow positions clamp inside the rail', () => {
  assert.equal(conversationTurnPageControlTop(5, -1, 100), 0);
  assert.equal(conversationTurnPageControlTop(95, 1, 100), 80);
});

run('nearest hit resolves a marker only inside the allowed distance', () => {
  const layout = conversationTurnMarkerLayout(5, 120);
  assert.equal(nearestConversationTurnIndex(44, layout, 9), 1);
  assert.equal(nearestConversationTurnIndex(5, layout, 9), -1);
});

run('expanded marker pushes every marker above and below outward as ordered groups', () => {
  assert.deepEqual(
    Array.from(
      { length: 7 },
      (_, index) => conversationTurnMarkerDisplacement(index, 3, 7),
    ),
    [-8, -8, -8, 0, 8, 8, 8],
  );
});

run('marker displacement is zero at rest and for invalid indices', () => {
  assert.equal(conversationTurnMarkerDisplacement(2, -1, 5), 0);
  assert.equal(conversationTurnMarkerDisplacement(6, 2, 5), 0);
  assert.equal(conversationTurnMarkerDisplacement(2, 2, 5), 0);
});

run('outward group displacement preserves order in a densely compressed rail', () => {
  const layout = conversationTurnMarkerLayout(40, 48);
  const expandedIndex = 20;
  const shiftedCenters = layout.map((marker) => (
    marker.centerY + conversationTurnMarkerDisplacement(
      marker.index,
      expandedIndex,
      layout.length,
    )
  ));
  assert.ok(
    shiftedCenters.every((center, index) => (
      index === 0 || center >= shiftedCenters[index - 1]
    )),
  );
  assert.ok(
    shiftedCenters[expandedIndex] - shiftedCenters[expandedIndex - 1] > 8,
  );
  assert.ok(
    shiftedCenters[expandedIndex + 1] - shiftedCenters[expandedIndex] > 8,
  );
});

run('active turn follows the latest user row above the scroll probe', () => {
  const turns = [
    { itemId: 'user-1' },
    { itemId: 'user-2' },
    { itemId: 'user-3' },
  ];
  const rows = [
    { id: 'user-1', top: 10, bottom: 80 },
    { id: 'user-2', top: 240, bottom: 320 },
    { id: 'user-3', top: 470, bottom: 550 },
  ];
  assert.equal(activeConversationTurnIndex(turns, rows, 230), 1);
  assert.equal(activeConversationTurnIndex(turns, rows, 0), 0);
});

run('clicked near-tail turn stays active while a clamped jump cannot move farther', () => {
  const turns = Array.from(
    { length: 5 },
    (_, index) => ({ itemId: `user-${index + 1}` }),
  );
  const rows = [
    { id: 'user-1', top: 10, bottom: 60 },
    { id: 'user-2', top: 90, bottom: 140 },
    { id: 'user-3', top: 170, bottom: 220 },
    { id: 'user-4', top: 520, bottom: 570 },
    { id: 'user-5', top: 840, bottom: 890 },
  ];
  const clampedScrollTop = 200;

  assert.equal(activeConversationTurnIndex(turns, rows, clampedScrollTop), 2);
  assert.equal(activatedConversationTurnIndex(
    turns,
    { itemId: 'user-5', scrollTop: clampedScrollTop },
    clampedScrollTop,
  ), 4);
});

run('clicked turn activation releases after the scroll position actually changes', () => {
  const turns = [
    { itemId: 'user-1' },
    { itemId: 'user-2' },
  ];
  const activation = { itemId: 'user-2', scrollTop: 200 };

  assert.equal(activatedConversationTurnIndex(turns, activation, 200.5), 1);
  assert.equal(activatedConversationTurnIndex(turns, activation, 180), -1);
  assert.equal(activatedConversationTurnIndex(
    turns,
    { itemId: 'missing', scrollTop: 200 },
    200,
  ), -1);
});

run('preview card top stays clamped inside the rail', () => {
  assert.equal(conversationTurnPreviewTop(10, 300, 100), 8);
  assert.equal(conversationTurnPreviewTop(290, 300, 100), 192);
});
