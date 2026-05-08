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

run('usage 事件更新 token usage 且不新增 transcript item', () => {
  const state = reduceMany([
    {
      type: 'usage',
      payload: { prompt_tokens: 8000, completion_tokens: 1200, total_tokens: 9200, has_data: true },
      timestamp_ms: 123,
      seq: 1,
    },
  ]);
  assert.equal(state.items.length, 0);
  assert.deepEqual(state.tokenUsage, {
    promptTokens: 8000,
    completionTokens: 1200,
    totalTokens: 9200,
    hasData: true,
    timestampMs: 123,
  });
  assert.equal(state.lastSeq, 1);
});

run('history replay 会恢复 usage 事件状态', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [{ id: 'u1', role: 'user', content: 'hello', ts: 1 }],
    events: [
      {
        type: 'usage',
        payload: { prompt_tokens: 64000, completion_tokens: 500, total_tokens: 64500, has_data: true },
        timestamp_ms: 456,
        seq: 2,
      },
    ],
  }).state;
  assert.equal(loaded.items.length, 1);
  assert.equal(loaded.tokenUsage.promptTokens, 64000);
  assert.equal(loaded.tokenUsage.completionTokens, 500);
  assert.equal(loaded.tokenUsage.totalTokens, 64500);
  assert.equal(loaded.tokenUsage.hasData, true);
  assert.equal(loaded.tokenUsage.timestampMs, 456);
  assert.equal(loaded.lastSeq, 2);
});

run('history load 将带 tool_hunks metadata 的 tool message 恢复为 tool item', () => {
  const hunk = { old_start: 1, old_count: 1, new_start: 1, new_count: 2, lines: [] };
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'change file', ts: 1 },
      {
        id: 't1',
        role: 'tool',
        content: 'edited',
        ts: 2,
        metadata: {
          tool_summary: { verb: 'Edit', object: 'src/a.js', icon: 'edit', metrics: [['+', '2'], ['-', '1']] },
          tool_hunks: [hunk],
        },
      },
    ],
    events: [],
  }).state;
  assert.equal(loaded.items.length, 2);
  assert.equal(loaded.items[1].kind, 'tool');
  assert.equal(loaded.items[1].tool.summary.object, 'src/a.js');
  assert.deepEqual(loaded.items[1].tool.summary.metrics, [
    { label: '+', value: '2' },
    { label: '-', value: '1' },
  ]);
  assert.deepEqual(loaded.items[1].tool.hunks, [hunk]);
});

run('新 transcript token usage 默认为 unknown 且不跨 session 继承', () => {
  const previous = reduceMany([
    { type: 'usage', payload: { prompt_tokens: 8000, completion_tokens: 1, total_tokens: 8001, has_data: true }, seq: 1 },
  ]);
  assert.equal(previous.tokenUsage.promptTokens, 8000);

  const fresh = createTranscriptState({ title: 'next' });
  assert.equal(fresh.tokenUsage, null);

  const loaded = loadTranscriptHistory(previous, { messages: [], events: [] }).state;
  assert.equal(loaded.tokenUsage, null);
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

run('agent_progress 更新活动状态且 busy 结束时清理', () => {
  const state = reduceMany([
    { type: 'busy_changed', payload: { busy: true }, seq: 1 },
    {
      type: 'agent_progress',
      payload: {
        phase: 'tool_planning',
        label: '正在准备调用 grep',
        detail: '参数 2 KB',
        tool: 'grep',
        tool_call_id: 'call-1',
        tool_index: 0,
        started_at_ms: 1000,
      },
      timestamp_ms: 1200,
      seq: 2,
    },
  ]);
  assert.equal(state.activity.phase, 'tool_planning');
  assert.equal(state.activity.tool, 'grep');
  assert.equal(state.activity.toolCallId, 'call-1');
  assert.equal(state.activity.startedAtMs, 1000);

  const cleared = reduceTranscriptEvent(state, { type: 'busy_changed', payload: { busy: false }, seq: 3 }).state;
  assert.equal(cleared.activity, null);
});

run('tool_call_id 优先关联并行同名工具', () => {
  const state = reduceMany([
    { type: 'tool_start', payload: { tool: 'grep', tool_call_id: 'call-a', tool_index: 0 }, seq: 1 },
    { type: 'tool_start', payload: { tool: 'grep', tool_call_id: 'call-b', tool_index: 1 }, seq: 2 },
    { type: 'tool_update', payload: { tool: 'grep', tool_call_id: 'call-b', tail_lines: ['B'], total_lines: 1 }, seq: 3 },
    { type: 'tool_end', payload: { tool: 'grep', tool_call_id: 'call-a', success: true }, seq: 4 },
    { type: 'tool_end', payload: { tool: 'grep', tool_call_id: 'call-b', success: false, output: 'bad' }, seq: 5 },
  ]);
  assert.equal(state.items.length, 2);
  const [a, b] = state.items.map((item) => item.tool);
  assert.equal(a.toolCallId, 'call-a');
  assert.equal(b.toolCallId, 'call-b');
  assert.equal(a.isDone, true);
  assert.equal(a.success, true);
  assert.deepEqual(b.tailLines, ['B']);
  assert.equal(b.success, false);
  assert.equal(b.output, 'bad');
});

run('reasoning 事件不追加到可见 assistant 消息', () => {
  const state = reduceMany([
    { type: 'reasoning', payload: { text: 'hidden thought' }, seq: 1 },
    { type: 'token', payload: { text: 'visible' }, seq: 2 },
  ]);
  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].role, 'assistant');
  assert.equal(state.items[0].content, 'visible');
});

run('新 session 不继承上一 session 活动状态', () => {
  const previous = reduceMany([
    { type: 'busy_changed', payload: { busy: true }, seq: 1 },
    { type: 'agent_progress', payload: { phase: 'model_waiting', label: '等待' }, seq: 2 },
  ]);
  assert.equal(previous.activity.phase, 'model_waiting');

  const fresh = createTranscriptState({ title: 'next' });
  assert.equal(fresh.activity, null);

  const loaded = loadTranscriptHistory(previous, { messages: [], events: [] }).state;
  assert.equal(loaded.activity, null);
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
