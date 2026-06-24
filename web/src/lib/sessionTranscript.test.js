import assert from 'node:assert/strict';
import {
  applyTranscriptReplayEvents,
  canLiveMonitorSession,
  createTranscriptState,
  loadTranscriptHistory,
  projectCompactTranscriptItems,
  reduceTranscriptEvent,
  replaySinceForLiveCatchup,
} from './sessionTranscript.js';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';

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

function persistedToolCall(id, name, argumentsText, index = undefined) {
  const call = {
    id,
    type: 'function',
    function: { name, arguments: argumentsText },
  };
  if (index != null) call.index = index;
  return call;
}

function projectLoadedItems(items) {
  return projectCollapsedTranscriptItems(items, { deferTrailingToolSummary: true });
}

function turnTimingMessage(userMessageUuid, durationMs, status = 'completed') {
  return {
    id: `timing-${userMessageUuid}`,
    role: 'system',
    content: '[Turn timing]',
    timestamp: '2026-06-04T00:00:05Z',
    metadata: {
      transcript_only: true,
      turn_timing: {
        user_message_uuid: userMessageUuid,
        started_at_ms: 1000,
        completed_at_ms: 1000 + durationMs,
        duration_ms: durationMs,
        status,
      },
    },
  };
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

run('history load 使用持久 timestamp 而不是页面加载时间', () => {
  const userTs = '2026-05-01T01:02:03Z';
  const assistantTs = '2026-05-01T01:02:05Z';
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'old prompt', timestamp: userTs },
      { id: 'a1', role: 'assistant', content: 'old answer', timestamp: assistantTs },
    ],
    events: [],
  }).state;

  assert.equal(loaded.items[0].ts, Date.parse(userTs));
  assert.equal(loaded.items[1].ts, Date.parse(assistantTs));
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

run('assistant final message 覆盖尾部已结束的 stream draft', () => {
  const draft = reduceMany([
    { type: 'token', payload: { text: '### 2直接挂\n```' }, seq: 1 },
    { type: 'done', payload: {}, seq: 2 },
  ]);
  assert.equal(draft.streamingId, null);
  assert.equal(draft.items[0].streamDraft, true);
  assert.equal(draft.items[0].streaming, false);

  const state = reduceTranscriptEvent(draft, {
    type: 'message',
    payload: {
      id: 'a1',
      role: 'assistant',
      content: '2. `mkdir && move` 一步执行直接挂\n\n```cmd\nmkdir docs && move file docs\n```',
    },
    seq: 3,
  }).state;

  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].messageId, 'a1');
  assert.equal(state.items[0].streamDraft, false);
  assert.equal(state.items[0].content, '2. `mkdir && move` 一步执行直接挂\n\n```cmd\nmkdir docs && move file docs\n```');
});

run('assistant final message 不跨 tool 边界替换旧 stream draft', () => {
  const state = reduceMany([
    { type: 'token', payload: { text: '先检查' }, seq: 1 },
    { type: 'tool_start', payload: { tool: 'bash', tool_call_id: 'call-1', args: { cmd: 'pwd' } }, seq: 2 },
    { type: 'message', payload: { id: 'a1', role: 'assistant', content: '最终答案' }, seq: 3 },
  ]);

  assert.equal(state.items.length, 3);
  assert.equal(state.items[0].content, '先检查');
  assert.equal(state.items[0].streaming, false);
  assert.equal(state.items[1].kind, 'tool');
  assert.equal(state.items[2].content, '最终答案');
});

run('重复 replay 的 token seq 不会重复追加 streaming 文本', () => {
  let state = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'work', ts: 1 },
    ],
    events: [
      { type: 'token', payload: { text: 'hel' }, seq: 2 },
      { type: 'token', payload: { text: 'lo' }, seq: 3 },
    ],
  }).state;

  state = reduceTranscriptEvent(state, { type: 'token', payload: { text: 'hel' }, seq: 2 }).state;
  state = reduceTranscriptEvent(state, { type: 'token', payload: { text: 'lo' }, seq: 3 }).state;
  state = reduceTranscriptEvent(state, { type: 'token', payload: { text: '!' }, seq: 4 }).state;

  assert.equal(state.lastSeq, 4);
  assert.deepEqual(state.items.map((item) => item.content), ['work', 'hello!']);
});

run('history load 不继承被旧快照覆盖的 live seq 水位', () => {
  const live = reduceMany([
    { type: 'message', payload: { id: 'u1', role: 'user', content: 'work' }, seq: 1 },
    { type: 'token', payload: { text: 'partial' }, seq: 2 },
  ]);

  const loaded = loadTranscriptHistory(live, {
    messages: [
      { id: 'u1', role: 'user', content: 'work', ts: 1 },
    ],
    events: [],
  }).state;

  assert.equal(loaded.lastSeq, 0);
  const final = reduceTranscriptEvent(loaded, {
    type: 'message',
    payload: { id: 'a1', role: 'assistant', content: 'final answer' },
    seq: 2,
  }).state;
  assert.deepEqual(final.items.map((item) => item.content), ['work', 'final answer']);
});

run('live 首次加载后从 1 补拉事件缓冲,避开 since=0 不回放', () => {
  assert.equal(replaySinceForLiveCatchup({ isLive: false, loadedSeq: 0 }), null);
  assert.equal(replaySinceForLiveCatchup({ isLive: true, loadedSeq: 0 }), 1);
  assert.equal(replaySinceForLiveCatchup({ isLive: true, loadedSeq: 42 }), 42);
});

run('live catch-up replay 丢弃已持久化 message 前的旧 token', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'work', ts: 1 },
      { id: 'a1', role: 'assistant', content: 'final persisted', ts: 2 },
    ],
    events: [],
  }).state;

  const replayed = applyTranscriptReplayEvents(loaded, [
    { type: 'token', payload: { text: 'partial stale ' }, seq: 2 },
    { type: 'message', payload: { id: 'a1', role: 'assistant', content: 'final persisted' }, seq: 3 },
  ]).state;

  assert.deepEqual(replayed.items.map((item) => item.content), ['work', 'final persisted']);
  assert.equal(replayed.lastSeq, 3);
});

run('home auto_start 竞争:快照已含的 user 消息再经 WS 事件到达不重复(按 id 幂等)', () => {
  // 回归 bug(dedupe-message-events-by-id):主页发起首条消息时,daemon 的
  // GET /messages 先收集事件回放、后读消息快照,而回合线程先 append 消息
  // (中间隔落盘 I/O)、后 emit 事件;交错时响应呈现「messages 已含 user
  // 消息、events 不含对应事件」,seq 水位未推进,随后该 message 事件经 WS
  // 送达 → 旧实现无条件追加,用户气泡出现两次。
  // 期望:按持久消息 id 命中快照条目,原位更新(吸收事件的 metadata),不追加。
  let state = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'hello', ts: 1 },
    ],
    events: [],
  }).state;
  // 竞争窗口的关键前提:快照未带事件,水位仍为 0,后到事件不会被 seq 拦截
  assert.equal(state.lastSeq, 0);

  state = reduceTranscriptEvent(state, {
    type: 'message',
    payload: { id: 'u1', role: 'user', content: 'hello', metadata: { display_text: 'hello' } },
    seq: 1,
  }).state;

  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].messageId, 'u1');
  assert.equal(state.items[0].content, 'hello');
  assert.deepEqual(state.items[0].metadata, { display_text: 'hello' });
});

run('不同 id 的相同文本消息仍是两条(用户故意连发不被误删)', () => {
  // 触发场景:用户连续发送两条一字不差的消息,daemon 为它们生成不同的
  // 持久 UUID。期望:id 不同就不去重,transcript 显示两条。
  const state = reduceMany([
    { type: 'message', payload: { id: 'u1', role: 'user', content: '重复内容' }, seq: 1 },
    { type: 'message', payload: { id: 'u2', role: 'user', content: '重复内容' }, seq: 2 },
  ]);
  assert.equal(state.items.length, 2);
  assert.deepEqual(state.items.map((item) => item.messageId), ['u1', 'u2']);
});

run('无 id 的 message 事件维持追加行为', () => {
  // 期望行为:本地合成 / 老协议的无 id 消息不参与按 id 去重,照常追加。
  const state = reduceMany([
    { type: 'message', payload: { role: 'system', content: 'a' }, seq: 1 },
    { type: 'message', payload: { role: 'system', content: 'a' }, seq: 2 },
  ]);
  assert.equal(state.items.length, 2);
});

run('无 seq 的本地 transcript 事件仍会正常应用', () => {
  let state = reduceMany([
    { type: 'token', payload: { text: 'hello' }, seq: 1 },
  ]);
  state = reduceTranscriptEvent(state, { type: 'busy_changed', payload: { busy: true } }).state;
  assert.equal(state.lastSeq, 1);
  assert.equal(state.busy, true);
  assert.equal(state.status, 'running');
});

run('空 token 不创建 assistant streaming 占位', () => {
  const state = reduceMany([
    { type: 'busy_changed', payload: { busy: true }, seq: 1 },
    { type: 'agent_progress', payload: { phase: 'model_waiting', label: '等待模型' }, seq: 2 },
    { type: 'token', payload: { text: '' }, seq: 3 },
    { type: 'token', payload: { text: '  \n' }, seq: 4 },
  ]);

  assert.equal(state.items.length, 0);
  assert.equal(state.streamingId, null);
  assert.equal(state.activity.phase, 'model_waiting');
});

run('已有可见 assistant stream 后继续保留空白 token', () => {
  const state = reduceMany([
    { type: 'token', payload: { text: 'hello' }, seq: 1 },
    { type: 'token', payload: { text: '\n\n' }, seq: 2 },
    { type: 'token', payload: { text: 'world' }, seq: 3 },
  ]);

  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].content, 'hello\n\nworld');
  assert.notEqual(state.streamingId, null);
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
  assert.equal(state.items.at(-1).kind, 'termination_notice');
  assert.equal(state.items.at(-1).content, '任务已终止：boom');
});

run('session_updated 更新 transcript title', () => {
  const state = reduceTranscriptEvent(createTranscriptState({ title: 'Old title' }), {
    type: 'session_updated',
    payload: { title: 'New title', title_source: 'generated' },
    seq: 1,
  }).state;
  assert.equal(state.title, 'New title');
});

run('用户主动终止追加独立红色提示项', () => {
  const state = reduceMany([
    { type: 'busy_changed', payload: { busy: true }, seq: 1 },
    { type: 'turn_aborted', payload: { reason: '用户已终止本轮任务' }, seq: 2 },
  ]);

  assert.equal(state.busy, false);
  assert.equal(state.status, 'idle');
  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].kind, 'termination_notice');
  assert.equal(state.items[0].content, '用户已终止本轮任务');
});

run('用户终止后的 abort 类服务端错误不重复追加终止提示', () => {
  const state = reduceMany([
    { type: 'turn_aborted', payload: { reason: '用户已终止本轮任务' }, seq: 1 },
    { type: 'error', payload: { reason: 'aborted by user' }, seq: 2 },
  ]);

  assert.equal(state.items.filter((item) => item.kind === 'termination_notice').length, 1);
  assert.equal(state.items[0].content, '用户已终止本轮任务');
});

run('provider JSON 错误 message 保留完整展示文本和 metadata', () => {
  const prettyJson = '{\n  "unexpected": {\n    "nested": true\n  }\n}';
  const state = reduceTranscriptEvent(createTranscriptState(), {
    type: 'message',
    payload: {
      id: 'e1',
      role: 'error',
      content: `[Error] HTTP 400 from openai model test\n${prettyJson}`,
      metadata: {
        provider_error: {
          kind: 'http',
          status_code: 400,
          body_is_json: true,
          raw_body: '{"unexpected":{"nested":true}}',
          pretty_json: prettyJson,
        },
      },
    },
    seq: 1,
  }).state;

  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].role, 'error');
  assert.match(state.items[0].content, /"nested": true/);
  assert.equal(state.items[0].metadata.provider_error.raw_body, '{"unexpected":{"nested":true}}');
});

run('provider 非 JSON 错误 message 保留原始文本', () => {
  const rawBody = 'gateway says nope';
  const state = reduceTranscriptEvent(createTranscriptState(), {
    type: 'message',
    payload: {
      id: 'e1',
      role: 'error',
      content: `[Error] HTTP 400 from openai model test\n${rawBody}`,
      metadata: {
        provider_error: {
          kind: 'http',
          status_code: 400,
          body_is_json: false,
          raw_body: rawBody,
        },
      },
    },
    seq: 1,
  }).state;

  assert.equal(state.items.length, 1);
  assert.equal(state.items[0].role, 'error');
  assert.match(state.items[0].content, /gateway says nope/);
  assert.equal(state.items[0].metadata.provider_error.raw_body, rawBody);
});

run('provider 错误 message 会结束已有 partial assistant streaming 状态', () => {
  const state = reduceMany([
    { type: 'token', payload: { text: 'partial' }, seq: 1 },
    {
      type: 'message',
      payload: {
        id: 'e1',
        role: 'error',
        content: '[Error] stream ended before done',
        metadata: { provider_error: { kind: 'malformed_sse' } },
      },
      seq: 2,
    },
  ]);

  assert.equal(state.streamingId, null);
  assert.equal(state.items[0].role, 'assistant');
  assert.equal(state.items[0].streaming, false);
  assert.equal(state.items[1].role, 'error');
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

run('goal 事件更新和清理 transcript goal 状态', () => {
  const goal = {
    thread_id: 's1',
    goal_id: 'g1',
    objective: 'finish port',
    status: 'active',
    token_budget: 50000,
    tokens_used: 1200,
    remaining_tokens: 48800,
  };
  const state = reduceMany([
    { type: 'goal_updated', payload: { goal }, seq: 1 },
    { type: 'goal_cleared', payload: { session_id: 's1' }, seq: 2 },
  ]);

  assert.equal(state.items.length, 0);
  assert.equal(state.goal, null);
  assert.equal(state.lastSeq, 2);
});

run('history replay 恢复最近 goal 事件状态', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [],
    events: [
      {
        type: 'goal_updated',
        payload: {
          goal: {
            thread_id: 's1',
            goal_id: 'g1',
            objective: 'finish port',
            status: 'paused',
            tokens_used: 100,
          },
        },
        seq: 1,
      },
    ],
  }).state;

  assert.equal(loaded.goal.objective, 'finish port');
  assert.equal(loaded.goal.status, 'paused');
  assert.equal(loaded.lastSeq, 1);
});

run('todo_updated 事件更新 transcript checklist 状态', () => {
  const state = reduceTranscriptEvent(createTranscriptState(), {
    type: 'todo_updated',
    payload: {
      todos: [
        { id: '1', content: 'Inspect Hermes', status: 'completed' },
        { id: '2', content: 'Wire checklist UI', status: 'in_progress' },
      ],
      summary: { total: 2, completed: 1, in_progress: 1, pending: 0, cancelled: 0 },
    },
    seq: 1,
  }).state;

  assert.equal(state.todos.length, 2);
  assert.equal(state.todos[0].status, 'completed');
  assert.equal(state.todos[1].content, 'Wire checklist UI');
  assert.equal(state.todoSummary.total, 2);
  assert.equal(state.todoSummary.completed, 1);
});

run('history load 使用运行时快照恢复 todo checklist', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [],
    events: [],
    todos: [
      { id: '1', content: 'Done task', status: 'completed' },
      { id: '2', content: 'Next task', status: 'pending' },
    ],
    todo_summary: { total: 2, completed: 1, pending: 1 },
  }).state;

  assert.equal(loaded.todos.length, 2);
  assert.equal(loaded.todos[0].content, 'Done task');
  assert.equal(loaded.todoSummary.pending, 1);
});

run('history load 使用运行时快照恢复当前 goal 和 busy 状态', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [],
    events: [],
    goal: {
      thread_id: 's1',
      goal_id: 'g1',
      objective: 'finish port',
      status: 'active',
      tokens_used: 100,
    },
    busy: true,
  }).state;

  assert.equal(loaded.goal.objective, 'finish port');
  assert.equal(loaded.goal.status, 'active');
  assert.equal(loaded.busy, true);
  assert.equal(loaded.status, 'running');
});

run('history load 使用运行时快照恢复轮次和 token 状态', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [{ id: 'u1', role: 'user', content: 'hello', ts: 1 }],
    events: [],
    turn_count: 5,
    token_usage: {
      prompt_tokens: 32000,
      completion_tokens: 1200,
      total_tokens: 33200,
      has_data: true,
    },
  }).state;

  assert.equal(loaded.turns, 5);
  assert.equal(loaded.tokenUsage.promptTokens, 32000);
  assert.equal(loaded.tokenUsage.completionTokens, 1200);
  assert.equal(loaded.tokenUsage.totalTokens, 33200);
  assert.equal(loaded.tokenUsage.hasData, true);
});

run('history load 的 idle 运行时快照不增加轮次', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1', turns: 3 }), {
    messages: [],
    events: [],
    busy: false,
  }).state;

  assert.equal(loaded.busy, false);
  assert.equal(loaded.status, 'idle');
  assert.equal(loaded.turns, 0);
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

run('history load 将 AskUserQuestion metadata 恢复为确认卡片工具项', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'confirm details', ts: 1 },
      {
        id: 't1',
        role: 'tool',
        content: 'User has answered your questions: "Q?"="A"',
        tool_call_id: 'call-ask',
        ts: 2,
        metadata: {
          ask_user_question_result: {
            items: [
              { question: '希望我直接修改还是先给出方案让你确认?', answer: '直接修改并补测试' },
            ],
          },
        },
      },
    ],
    events: [],
  }).state;
  assert.equal(loaded.items.length, 2);
  assert.equal(loaded.items[1].kind, 'tool');
  assert.equal(loaded.items[1].tool.toolCallId, 'call-ask');
  assert.deepEqual(loaded.items[1].tool.askUserQuestionResult.items, [
    { question: '希望我直接修改还是先给出方案让你确认?', answer: '直接修改并补测试' },
  ]);
});

run('history load 将带 output attachment 的 tool message 恢复为 tool item', () => {
  const attachment = {
    id: 'att-img',
    name: 'plot.png',
    kind: 'image',
    mime_type: 'image/png',
    blob_url: '/api/sessions/s1/attachments/att-img/blob',
  };
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'make plot', ts: 1 },
      {
        id: 't1',
        role: 'tool',
        content: 'created plot',
        tool_call_id: 'call-plot',
        content_parts: [{ type: 'image', attachment }],
        ts: 2,
      },
    ],
    events: [],
  }).state;
  assert.equal(loaded.items.length, 2);
  assert.equal(loaded.items[1].kind, 'tool');
  assert.equal(loaded.items[1].tool.toolCallId, 'call-plot');
  assert.deepEqual(loaded.items[1].tool.attachments, [{ ...attachment, type: 'image' }]);
});

run('history load 展开 persisted assistant.tool_calls 并匹配无 summary 工具返回', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'run date', ts: 1 },
      {
        id: 'a1',
        role: 'assistant',
        content: '',
        tool_calls: [persistedToolCall('call-date', 'shell_command', '{"command":"date"}', 0)],
        ts: 2,
      },
      {
        id: 't1',
        role: 'tool',
        content: 'Thu Jun  4 12:00:00 CST 2026',
        tool_call_id: 'call-date',
        tool_index: 0,
        ts: 3,
      },
    ],
    events: [],
  }).state;

  assert.deepEqual(loaded.items.map((item) => item.role), ['user', 'tool_call', 'tool']);
  assert.equal(loaded.items[1].messageId, 'a1:tool_call:call-date');
  assert.equal(loaded.items[1].content, '[Tool: shell_command] {"command":"date"}');
  assert.equal(loaded.items[1].tool_call_id, 'call-date');
  assert.equal(loaded.items[1].tool_index, 0);
  assert.equal(loaded.items[2].tool_call_id, 'call-date');
  assert.equal(loaded.items[2].tool_index, 0);

  const projected = projectLoadedItems(loaded.items);
  const serialized = JSON.stringify(projected);
  assert.equal(serialized.includes('请求未记录'), false);
  assert.match(projected[1].content, /\[Tool: shell_command\] \{"command":"date"\}/);
  assert.match(projected[1].content, /Thu Jun  4 12:00:00 CST 2026/);
});

run('transcript_replace 展开 persisted assistant.tool_calls 并匹配无 summary 工具返回', () => {
  const previous = reduceMany([
    { type: 'message', payload: { id: 'old', role: 'user', content: 'old prompt' }, seq: 1 },
    { type: 'token', payload: { text: 'partial' }, seq: 2 },
  ]);
  const state = reduceTranscriptEvent(previous, {
    type: 'transcript_replace',
    payload: {
      messages: [
        { id: 'u1', role: 'user', content: 'list files', ts: 1 },
        {
          id: 'a1',
          role: 'assistant',
          content: '',
          tool_calls: [persistedToolCall('call-ls', 'shell_command', '{"command":"dir"}', 0)],
          ts: 2,
        },
        { id: 't1', role: 'tool', content: 'README.md', tool_call_id: 'call-ls', tool_index: 0, ts: 3 },
      ],
    },
    seq: 3,
  }).state;

  assert.equal(state.streamingId, null);
  assert.deepEqual(state.items.map((item) => item.role), ['user', 'tool_call', 'tool']);
  assert.equal(state.items[1].content, '[Tool: shell_command] {"command":"dir"}');
  assert.equal(state.items[2].toolCallId, 'call-ls');
  assert.equal(JSON.stringify(projectLoadedItems(state.items)).includes('请求未记录'), false);
});

run('history load 保持 assistant 文本在多个 persisted tool_calls 前面', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'inspect both', ts: 1 },
      {
        id: 'a1',
        role: 'assistant',
        content: 'I will inspect both files.',
        tool_calls: [
          persistedToolCall('call-a', 'file_read', '{"path":"a.txt"}', 0),
          persistedToolCall('call-b', 'file_read', '{"path":"b.txt"}', 1),
        ],
        ts: 2,
      },
    ],
    events: [],
  }).state;

  assert.deepEqual(loaded.items.map((item) => item.role), ['user', 'assistant', 'tool_call', 'tool_call']);
  assert.deepEqual(loaded.items.map((item) => item.content), [
    'inspect both',
    'I will inspect both files.',
    '[Tool: file_read] {"path":"a.txt"}',
    '[Tool: file_read] {"path":"b.txt"}',
  ]);
  assert.deepEqual(loaded.items.slice(2).map((item) => item.tool_call_id), ['call-a', 'call-b']);
  assert.deepEqual(loaded.items.slice(2).map((item) => item.tool_index), [0, 1]);
});

run('history load 同名并行 persisted tool_calls 按 tool_call_id 匹配各自返回', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'grep both', ts: 1 },
      {
        id: 'a1',
        role: 'assistant',
        content: '',
        tool_calls: [
          persistedToolCall('call-a', 'grep', '{"q":"A"}', 0),
          persistedToolCall('call-b', 'grep', '{"q":"B"}', 1),
        ],
        ts: 2,
      },
      { id: 'tb', role: 'tool', content: 'B result', tool_call_id: 'call-b', tool_index: 1, ts: 3 },
      { id: 'ta', role: 'tool', content: 'A result', tool_call_id: 'call-a', tool_index: 0, ts: 4 },
      { id: 'a2', role: 'assistant', content: 'done', ts: 5 },
    ],
    events: [],
  }).state;

  const projected = projectLoadedItems(loaded.items);
  const summary = projected.find((item) => item.kind === 'activity_summary');
  assert.ok(summary);
  assert.equal(summary.collapsedItems.length, 2);
  assert.deepEqual(summary.collapsedItems.map((item) => item.tool_call_id), ['call-a', 'call-b']);
  assert.match(summary.collapsedItems[0].content, /\[Tool: grep\] \{"q":"A"\}/);
  assert.match(summary.collapsedItems[0].content, /工具返回\nA result/);
  assert.match(summary.collapsedItems[1].content, /\[Tool: grep\] \{"q":"B"\}/);
  assert.match(summary.collapsedItems[1].content, /工具返回\nB result/);
  assert.equal(JSON.stringify(projected).includes('请求未记录'), false);
});

run('history load 对确实缺少请求的 tool result 保留请求未记录 fallback', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'legacy result only', ts: 1 },
      { id: 't1', role: 'tool', content: '{"ok":true}', ts: 2 },
      { id: 'a1', role: 'assistant', content: 'continue', ts: 3 },
    ],
    events: [],
  }).state;

  const projected = projectLoadedItems(loaded.items);
  assert.match(JSON.stringify(projected), /请求未记录/);
});

run('history load 消费 turn_timing 并用持久 duration 渲染 processed summary', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u-1', role: 'user', content: 'do work', timestamp: '2026-06-04T00:00:00Z' },
      { id: 'a-1', role: 'assistant', content: 'I will inspect first', timestamp: '2026-06-04T00:00:01Z' },
      {
        id: 'tool-1',
        role: 'tool',
        content: 'read file',
        timestamp: '2026-06-04T00:00:02Z',
        metadata: { tool_summary: { verb: 'Read', object: 'src/a.js', metrics: [] } },
      },
      {
        id: 'done-1',
        role: 'tool',
        tool: 'task_complete',
        content: 'done',
        timestamp: '2026-06-04T00:00:03Z',
        metadata: { tool_summary: { verb: 'complete', object: 'task', metrics: [{ label: 'summary', value: 'done' }] } },
      },
      turnTimingMessage('u-1', 65000),
    ],
    events: [],
  }).state;

  assert.equal(loaded.items.some((item) => item.content === '[Turn timing]'), false);
  const projected = projectLoadedItems(loaded.items);
  assert.equal(projected[1].kind, 'activity_summary');
  assert.equal(projected[1].mode, 'processed');
  assert.equal(projected[1].title, '已处理 1m 5s');
  assert.equal(projected[2].kind, 'completion_summary');

  const reloaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u-1', role: 'user', content: 'do work', timestamp: '2026-06-04T00:00:00Z' },
      { id: 'a-1', role: 'assistant', content: 'I will inspect first', timestamp: '2026-06-04T00:00:01Z' },
      {
        id: 'tool-1',
        role: 'tool',
        content: 'read file',
        timestamp: '2026-06-04T00:00:02Z',
        metadata: { tool_summary: { verb: 'Read', object: 'src/a.js', metrics: [] } },
      },
      {
        id: 'done-1',
        role: 'tool',
        tool: 'task_complete',
        content: 'done',
        timestamp: '2026-06-04T00:00:03Z',
        metadata: { tool_summary: { verb: 'complete', object: 'task', metrics: [{ label: 'summary', value: 'done' }] } },
      },
      turnTimingMessage('u-1', 65000),
    ],
    events: [],
  }).state;
  assert.equal(projectLoadedItems(reloaded.items)[1].title, '已处理 1m 5s');
});

run('history load 没有 turn_timing 时保留 timestamp fallback', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u-1', role: 'user', content: 'do work', timestamp: '2026-06-04T00:00:00Z' },
      { id: 'a-1', role: 'assistant', content: 'I will inspect first', timestamp: '2026-06-04T00:00:01Z' },
      {
        id: 'tool-1',
        role: 'tool',
        content: 'read file',
        timestamp: '2026-06-04T00:00:02Z',
        metadata: { tool_summary: { verb: 'Read', object: 'src/a.js', metrics: [] } },
      },
      {
        id: 'done-1',
        role: 'tool',
        tool: 'task_complete',
        content: 'done',
        timestamp: '2026-06-04T00:00:03Z',
        metadata: { tool_summary: { verb: 'complete', object: 'task', metrics: [{ label: 'summary', value: 'done' }] } },
      },
    ],
    events: [],
  }).state;

  const projected = projectLoadedItems(loaded.items);
  assert.equal(projected[1].title, '已处理 2s');
});

run('history load error timing 不把 termination notice 折叠进 processed summary', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u-1', role: 'user', content: 'do work', timestamp: '2026-06-04T00:00:00Z' },
      { id: 'a-1', role: 'assistant', content: 'I will inspect first', timestamp: '2026-06-04T00:00:01Z' },
      {
        id: 'tool-1',
        role: 'tool',
        content: 'read file',
        timestamp: '2026-06-04T00:00:02Z',
        metadata: { tool_summary: { verb: 'Read', object: 'src/a.js', metrics: [] } },
      },
      { id: 'a-2', role: 'assistant', content: 'partial answer', timestamp: '2026-06-04T00:00:03Z' },
      turnTimingMessage('u-1', 3000, 'error'),
    ],
    events: [
      { type: 'error', payload: { reason: 'provider failed' }, seq: 1 },
    ],
  }).state;

  const projected = projectLoadedItems(loaded.items);
  const notice = projected.at(-1);
  assert.equal(notice.kind, 'termination_notice');
  assert.equal(notice.content, '任务已终止：provider failed');
  assert.equal(projected.some((item) => item.kind === 'activity_summary' && item.collapsedItems?.includes(notice)), false);
  assert.equal(projected.some((item) => item.content === '[Turn timing]'), false);
});

run('history load 不显示内部 meta 消息', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'm1', role: 'system', content: '[Compact boundary]', is_meta: true },
      { id: 's1', role: 'system', content: '[Conversation summary]\nold prompt summarized' },
      { id: 'u1', role: 'user', content: 'kept prompt' },
    ],
    events: [],
  }).state;
  assert.deepEqual(loaded.items.map((item) => item.content), [
    '[Conversation summary]\nold prompt summarized',
    'kept prompt',
  ]);
});

run('history load 不显示隐藏 goal context 消息', () => {
  const loaded = loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages: [
      { id: 'u1', role: 'user', content: 'visible prompt' },
      {
        id: 'g1',
        role: 'user',
        content: '<goal_context>continue</goal_context>',
        metadata: { hidden_goal_context: true },
      },
      { id: 'a1', role: 'assistant', content: 'visible answer' },
    ],
    events: [],
  }).state;

  assert.deepEqual(loaded.items.map((item) => item.content), ['visible prompt', 'visible answer']);
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

run('tool lifecycle 保留进度、summary、失败输出、hunks 和附件', () => {
  const hunk = { file: 'a.txt', old_start: 1, old_lines: ['a'], new_start: 1, new_lines: ['b'] };
  const attachment = { id: 'att-img', name: 'screen.png', mime_type: 'image/png', kind: 'image' };
  const state = reduceMany([
    { type: 'tool_start', payload: { tool: 'file_edit', display_override: 'edit a.txt' }, seq: 1 },
    { type: 'tool_update', payload: { tool: 'file_edit', tail_lines: ['patching'], total_lines: 1, total_bytes: 8, elapsed_seconds: 2 }, seq: 2 },
    { type: 'tool_end', payload: { tool: 'file_edit', success: false, summary: { verb: 'Edit', object: 'a.txt' }, output: 'failed', hunks: [hunk], attachments: [attachment] }, seq: 3 },
  ]);
  assert.equal(state.items.length, 1);
  const tool = state.items[0].tool;
  assert.equal(tool.isDone, true);
  assert.equal(tool.success, false);
  assert.equal(tool.summary.object, 'a.txt');
  assert.equal(tool.output, 'failed');
  assert.deepEqual(tool.hunks, [hunk]);
  assert.deepEqual(tool.attachments, [attachment]);
});

run('tool_start 保留原始 args 供工具块展开显示完整命令', () => {
  const command = 'cd C:\\Users\\shao\\acecode && git log --oneline --since="2026-01-01" | wc -l';
  const state = reduceMany([
    {
      type: 'tool_start',
      payload: {
        tool: 'bash',
        args: { command },
        display_override: 'bash  cd C:\\Users\\sh...',
        command_preview: 'cd C:\\Users\\sh...',
      },
      seq: 1,
    },
    {
      type: 'tool_end',
      payload: {
        tool: 'bash',
        success: false,
        summary: { verb: 'Ran', object: 'cd C:\\Users\\sh...' },
        output: "'wc' 不是内部或外部命令",
      },
      seq: 2,
    },
  ]);
  const tool = state.items[0].tool;
  assert.equal(tool.displayOverride, 'bash  cd C:\\Users\\sh...');
  assert.equal(tool.summary.object, 'cd C:\\Users\\sh...');
  assert.equal(tool.args.command, command);
});

run('tool lifecycle 保留 AskUserQuestion 确认结果 metadata', () => {
  const state = reduceMany([
    { type: 'tool_start', payload: { tool: 'AskUserQuestion', tool_call_id: 'call-ask' }, seq: 1 },
    {
      type: 'tool_end',
      payload: {
        tool: 'AskUserQuestion',
        tool_call_id: 'call-ask',
        success: true,
        metadata: {
          ask_user_question_result: {
            items: [
              { question: '是否直接修改?', answer: '直接修改并补测试' },
              { question: '清理时机?', answer: 'onBeforeUnmount' },
            ],
          },
        },
      },
      seq: 2,
    },
  ]);
  assert.equal(state.items.length, 1);
  const tool = state.items[0].tool;
  assert.equal(tool.isDone, true);
  assert.equal(tool.success, true);
  assert.equal(tool.askUserQuestionResult.items.length, 2);
  assert.equal(tool.askUserQuestionResult.items[1].answer, 'onBeforeUnmount');
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

run('transcript_replace 替换可见 transcript 并清理 token usage', () => {
  const previous = reduceMany([
    { type: 'message', payload: { id: 'u-old', role: 'user', content: 'old prompt' }, seq: 1 },
    { type: 'usage', payload: { prompt_tokens: 1000, total_tokens: 1000, has_data: true }, seq: 2 },
    { type: 'busy_changed', payload: { busy: true }, seq: 3 },
  ]);
  assert.equal(previous.items.length, 1);
  assert.equal(previous.tokenUsage.promptTokens, 1000);

  const state = reduceMany([
    {
      type: 'transcript_replace',
      payload: {
        messages: [
          { id: 'meta', role: 'system', content: '[Compact boundary]', is_meta: true },
          { id: 'summary', role: 'system', content: '[Conversation summary]\nold prompt summarized' },
          { id: 'u-keep', role: 'user', content: 'kept prompt' },
        ],
      },
      seq: 4,
    },
    {
      type: 'message',
      payload: { role: 'system', content: 'Compacted 2 messages, saved ~400 tokens.' },
      seq: 5,
    },
    { type: 'busy_changed', payload: { busy: false }, seq: 6 },
  ], previous);

  assert.deepEqual(state.items.map((item) => item.content), [
    '[Conversation summary]\nold prompt summarized',
    'kept prompt',
    'Compacted 2 messages, saved ~400 tokens.',
  ]);
  assert.equal(state.items.some((item) => item.content === 'old prompt'), false);
  assert.equal(state.tokenUsage, null);
  assert.equal(state.busy, false);
  assert.equal(state.status, 'idle');
});

run('compact marker 追加后保留旧消息', () => {
  const previous = reduceMany([
    { type: 'message', payload: { id: 'u-old', role: 'user', content: 'old prompt' }, seq: 1 },
    { type: 'message', payload: { id: 'a-old', role: 'assistant', content: 'old response' }, seq: 2 },
  ]);

  const state = reduceMany([
    {
      type: 'message',
      payload: {
        id: 'compact-marker',
        role: 'system',
        content: 'Compacted 2 messages, saved ~400 tokens.',
        metadata: { transcript_only: true },
      },
      seq: 3,
    },
  ], previous);

  assert.deepEqual(state.items.map((item) => item.content), [
    'old prompt',
    'old response',
    'Compacted 2 messages, saved ~400 tokens.',
  ]);
});

run('transcript_replace 清理正在流式输出和活动工具映射', () => {
  const previous = reduceMany([
    { type: 'token', payload: { text: 'partial' }, seq: 1 },
  ]);
  assert.notEqual(previous.streamingId, null);
  const withToolMap = {
    ...previous,
    toolMap: new Map([['call-1', 42]]),
  };

  const state = reduceTranscriptEvent(withToolMap, {
    type: 'transcript_replace',
    payload: { messages: [{ id: 'u1', role: 'user', content: 'after compact' }] },
    seq: 3,
  }).state;

  assert.equal(state.streamingId, null);
  assert.equal(state.toolMap.size, 0);
  assert.deepEqual(state.items.map((item) => item.content), ['after compact']);
});

run('timeout retry 的 transcript_replace 丢弃失败连接的 partial token', () => {
  const previous = reduceMany([
    { type: 'message', payload: { id: 'u1', role: 'user', content: 'work' }, seq: 1 },
    { type: 'token', payload: { text: 'partial from timed out stream' }, seq: 2 },
  ]);

  const reset = reduceTranscriptEvent(previous, {
    type: 'transcript_replace',
    payload: { messages: [{ id: 'u1', role: 'user', content: 'work' }] },
    seq: 3,
  }).state;
  const final = reduceMany([
    { type: 'token', payload: { text: 'final' }, seq: 4 },
    { type: 'message', payload: { id: 'a1', role: 'assistant', content: 'final' }, seq: 5 },
  ], reset);

  assert.equal(final.streamingId, null);
  assert.deepEqual(final.items.map((item) => item.content), ['work', 'final']);
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
