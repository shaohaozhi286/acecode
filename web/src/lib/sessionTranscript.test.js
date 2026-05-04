import assert from 'node:assert/strict';
import {
  canLiveMonitorSession,
  createTranscriptState,
  loadTranscriptHistory,
  projectCompactTranscriptItems,
  reduceTranscriptEvent,
} from './sessionTranscript.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function reduceMany(events, initial = createTranscriptState()) {
  return events.reduce((state, event) => reduceTranscriptEvent(state, event).state, initial);
}

run('history load 去重已持久化 replay message 并保留顺序', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'hello', ts: 1 },
      { id: 'a1', role: 'assistant', content: 'done', ts: 2 },
    ],
    events: [
      { type: 'token', payload: { text: 'do' }, seq: 3 },
      { type: 'message', payload: { id: 'a1', role: 'assistant', content: 'done' }, seq: 4 },
      { type: 'message', payload: { id: 'u2', role: 'user', content: 'next' }, seq: 5 },
    ],
  }).state;
  assert.equal(loaded.items.length, 3);
  assert.deepEqual(loaded.items.map((item) => item.content), ['hello', 'done', 'next']);
  assert.equal(loaded.lastSeq, 5);
});

run('assistant token streaming 被 final message 替换', () => {
  const state = reduceMany([
    { type: 'token', payload: { text: 'hel' }, seq: 1 },
    { type: 'token', payload: { text: 'lo' }, seq: 2 },
    { type: 'message', payload: { id: 'a1', role: 'assistant', content: 'hello final' }, seq: 3 },
  ]);
  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].content, 'hello final');
  assert.equal(state.items[0].streaming, false);
  assert.equal(state.streamingId, null);
});

run('busy done error 状态按事件更新', () => {
  let state = reduceMany([
    { type: 'busy_changed', payload: { busy: true }, seq: 1 },
    { type: 'busy_changed', payload: { busy: false }, seq: 2 },
  ]);
  assert.equal(state.busy, false);
  assert.equal(state.status, 'idle');
  assert.equal(state.turns, 1);

  state = reduceTranscriptEvent(state, { type: 'error', payload: { reason: 'boom' }, seq: 3 }).state;
  assert.equal(state.busy, false);
  assert.equal(state.status, 'error');
  assert.equal(state.error, 'boom');
});

run('tool lifecycle 保留进度、summary、失败输出和 hunks', () => {
  const hunk = { file: 'a.txt', old_start: 1, old_lines: ['a'], new_start: 1, new_lines: ['b'] };
  const state = reduceMany([
    { type: 'tool_start', payload: { tool: 'file_edit', display_override: 'edit a.txt' }, seq: 1 },
    { type: 'tool_update', payload: { tool: 'file_edit', tail_lines: ['patching'], total_lines: 1, total_bytes: 8, elapsed_seconds: 2 }, seq: 2 },
    { type: 'tool_end', payload: { tool: 'file_edit', success: false, summary: { verb: 'Edit', object: 'a.txt' }, output: 'failed', hunks: [hunk] }, seq: 3 },
  ]);
  assert.equal(state.items.length, 1);
  const tool = state.items[0].tool;
  assert.equal(tool.isDone, true);
  assert.equal(tool.success, false);
  assert.equal(tool.summary.object, 'a.txt');
  assert.equal(tool.output, 'failed');
  assert.deepEqual(tool.hunks, [hunk]);
});

run('compact projection 只取最近窗口且不改写 item 内容', () => {
  const items = Array.from({ length: 10 }, (_, index) => ({ kind: 'msg', id: index + 1, role: 'assistant', content: `m${index + 1}` }));
  const compact = projectCompactTranscriptItems(items, 4);
  assert.deepEqual(compact.map((item) => item.content), ['m7', 'm8', 'm9', 'm10']);
  assert.equal(compact[0], items[6]);
});

run('live/static 判定区分 active running 与磁盘历史', () => {
  assert.equal(canLiveMonitorSession({ id: 's1', active: true }), true);
  assert.equal(canLiveMonitorSession({ id: 's1', status: 'running' }), true);
  assert.equal(canLiveMonitorSession({ id: 's1', status: 'idle', active: false }), false);
  assert.equal(canLiveMonitorSession({ id: 's1' }, true), true);
  assert.equal(canLiveMonitorSession({ id: 's1', active: true }, false), false);
});
