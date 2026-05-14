import assert from 'node:assert/strict';
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

function user(id, content = 'do work', ts = id * 1000) {
  return { kind: 'msg', id, role: 'user', content, ts, messageId: `u-${id}` };
}

function assistant(id, content, ts = id * 1000, extra = {}) {
  return { kind: 'msg', id, role: 'assistant', content, ts, ...extra };
}

function tool(id, {
  name = 'file_read',
  verb = 'Read',
  object = `file-${id}.txt`,
  isDone = true,
  success = true,
  ts = id * 1000,
  hunks = [],
} = {}) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isDone,
      success,
      tool: name,
      summary: { verb, object, metrics: [] },
      output: success ? '' : 'command failed\nstderr details',
      hunks,
    },
  };
}

function taskComplete(id, summary = 'done', ts = id * 1000, success = true) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isTaskComplete: true,
      isDone: true,
      success,
      tool: 'task_complete',
      summary: { object: summary },
    },
  };
}

function completeSummaryTool(id, summary = 'done', ts = id * 1000) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isDone: true,
      success: true,
      tool: '',
      summary: {
        verb: 'complete',
        object: 'task',
        metrics: [{ label: 'summary', value: summary }],
      },
      output: '',
    },
  };
}

run('连续工具在 assistant 文本前折叠成一个 activity_summary', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Created', object: 'a.txt', name: 'file_write' }),
    tool(3, { verb: 'Edited', object: 'b.txt', name: 'file_edit' }),
    tool(4, { verb: 'Ran', object: 'pnpm test', name: 'bash' }),
    assistant(5, 'done'),
  ], { deferTrailingToolSummary: true });

  assert.equal(projected.length, 3);
  assert.equal(projected[1].kind, 'activity_summary');
  assert.equal(projected[1].mode, 'tools');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.match(projected[1].title, /已创建 1 个文件/);
  assert.match(projected[1].title, /已编辑 1 个文件/);
  assert.match(projected[1].title, /已运行 1 条命令/);
  assert.match(projected[1].title, /调用 3 个工具/);
  assert.equal(projected[2].content, 'done');
});

run('空 assistant 和工具调用返回行不会把连续工具拆成多个摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    assistant(2, ''),
    tool(3, { verb: 'Ran', object: 'cmd 1', name: 'bash' }),
    { kind: 'msg', id: 4, role: 'tool_result', content: 'ok', ts: 4000 },
    assistant(5, ''),
    tool(6, { verb: 'Ran', object: 'cmd 2', name: 'bash' }),
    { kind: 'msg', id: 7, role: 'tool_call', content: '[Tool: bash] cmd 3', ts: 7000 },
    tool(8, { verb: 'Ran', object: 'cmd 3', name: 'bash' }),
    assistant(9, 'all commands finished'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '已运行 3 条命令，调用 3 个工具');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4, 5, 6, 7, 8]);
  assert.equal(projected[2].content, 'all commands finished');
});

run('流式尾部工具段未遇到文字时保持原始连续工具', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    tool(3, { verb: 'Read', object: 'b.md', name: 'file_read' }),
    tool(4, { verb: 'Created', object: 'c.md', name: 'file_write' }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'tool', 'tool', 'tool']);
  assert.equal(projected[1].tool.summary.object, 'a.md');
  assert.equal(projected[2].tool.summary.object, 'b.md');
  assert.equal(projected[3].tool.summary.object, 'c.md');
});

run('流式尾部结构化工具隐藏冗余 tool_call 和 tool_result 包装行', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    { kind: 'msg', id: 2, role: 'tool_call', content: '[Tool: bash] {"command":"mkdir app"}', ts: 2000 },
    tool(3, { verb: 'Ran', object: 'mkdir app', name: 'bash' }),
    { kind: 'msg', id: 4, role: 'tool_result', content: '(no output)', ts: 4000 },
    { kind: 'msg', id: 5, role: 'tool_call', content: '[Tool: file_write] {"file_path":"app/index.html"}', ts: 5000 },
    tool(6, { verb: 'Created', object: 'app/index.html', name: 'file_write' }),
    { kind: 'msg', id: 7, role: 'tool_result', content: 'Created file: app/index.html', ts: 7000 },
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'tool', 'tool']);
  assert.deepEqual(projected.map((item) => item.role).filter(Boolean), ['user']);
  assert.equal(projected[1].tool.summary.object, 'mkdir app');
  assert.equal(projected[2].tool.summary.object, 'app/index.html');
});

run('没有结构化工具时保留 legacy tool wrapper 行', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    { kind: 'msg', id: 2, role: 'tool_call', content: '[Tool: legacy] {}', ts: 2000 },
    { kind: 'msg', id: 3, role: 'tool_result', content: 'legacy output', ts: 3000 },
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'msg', 'msg']);
  assert.deepEqual(projected.map((item) => item.role), ['user', 'tool_call', 'tool_result']);
});

run('连续 legacy 工具返回标签在 assistant 文本前折叠成工具摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    { kind: 'msg', id: 2, role: 'tool_result', content: '{"available_skills":["a","b"]}', ts: 2000 },
    { kind: 'msg', id: 3, role: 'tool_result', content: '{"available_categories":["general"]}', ts: 3000 },
    assistant(4, 'continue'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '调用 2 个工具');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
  assert.equal(projected[2].content, 'continue');
});

run('运行中的结构化工具隐藏前置 tool_call 包装行', () => {
  const running = tool(3, { isDone: false, name: 'bash', verb: 'Ran' });
  const projected = projectCollapsedTranscriptItems([
    user(1),
    { kind: 'msg', id: 2, role: 'tool_call', content: '[Tool: bash] {"command":"pnpm test"}', ts: 2000 },
    running,
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'tool']);
  assert.equal(projected[1], running);
});

run('流式遇到 assistant 文字时立刻合并前面的连续工具', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    tool(3, { verb: 'Read', object: 'b.md', name: 'file_read' }),
    tool(4, { verb: 'Created', object: 'c.md', name: 'file_write' }),
    assistant(5, 'I found the files', 5000, { streaming: true }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '已创建 1 个文件，读取 2 个文件，调用 3 个工具');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.equal(projected[2].content, 'I found the files');
});

run('流式工具之间的空白 assistant 占位不会拆分工具摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    assistant(3, ' ', 3000, { streaming: true }),
    tool(4, { verb: 'Read', object: 'b.md', name: 'file_read' }),
    assistant(5, '\n', 5000, { streaming: true }),
    tool(6, { verb: 'Created', object: 'c.md', name: 'file_write' }),
    assistant(7, 'Visible answer', 7000, { streaming: true }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '已创建 1 个文件，读取 2 个文件，调用 3 个工具');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4, 5, 6]);
  assert.equal(projected[2].content, 'Visible answer');
});

run('运行中工具和 streaming assistant 不折叠', () => {
  const running = tool(2, { isDone: false, name: 'bash', verb: 'Ran' });
  const streaming = assistant(3, 'partial', 3000, { streaming: true });
  const projected = projectCollapsedTranscriptItems([
    user(1),
    running,
    streaming,
  ]);

  assert.equal(projected.length, 3);
  assert.equal(projected[1], running);
  assert.equal(projected[2], streaming);
});

run('final assistant text 加 task_complete 时保留 final 并折叠前序活动', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    assistant(2, 'I will inspect first'),
    tool(3, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    tool(4, { verb: 'Edited', object: 'src/a.js', name: 'file_edit' }),
    assistant(5, 'Final answer kept'),
    taskComplete(6, 'all done'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.match(projected[1].title, /^已处理 /);
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.equal(projected[1].collapsedItems.length, 3);
  assert.equal(projected[2].content, 'Final answer kept');
  assert.equal(projected[3].title, '总结：all done');
  assert.equal(projected.some((item) => item.kind === 'tool' && item.tool?.isTaskComplete), false);
});

run('没有 final assistant text 时 task_complete 保留可展开处理摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    taskComplete(3, 'done'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.equal(projected[1].title, '已处理 1s');
  assert.deepEqual(projected[1].coveredItemIds, [2]);
  assert.equal(projected[2].title, '总结：done');
  assert.equal(projected.some((item) => item.kind === 'tool' && item.tool?.isTaskComplete), false);
});

run('complete task 摘要形态不计入工具并显示 summary metric', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    completeSummaryTool(3, '创建helloworld.txt文件，内容为helloworld'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.equal(projected[1].title, '已处理 1s');
  assert.equal(projected[2].title, '总结：创建helloworld.txt文件，内容为helloworld');
});

run('task_complete 的 tool_call 和 tool_result 包装行不会在 live 投影中露出', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'answers.md', name: 'file_read' }),
    assistant(3, 'Final answer'),
    {
      kind: 'msg',
      id: 4,
      role: 'tool_call',
      content: '[Tool: task_complete] {"summary":"created files"}',
      ts: 4000,
    },
    taskComplete(5, 'created files'),
    {
      kind: 'msg',
      id: 6,
      role: 'tool_result',
      content: 'created files',
      ts: 6000,
    },
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg', 'completion_summary']);
  assert.equal(projected[1].title, '已处理 3s');
  assert.equal(projected[2].content, 'Final answer');
  assert.equal(projected[3].title, '总结：created files');
  assert.equal(projected.some((item) => item.role === 'tool_call' || item.role === 'tool_result'), false);
});

run('失败工具折叠后外层不显示失败但 expanded 数据保留输出', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, {
      name: 'bash',
      verb: 'Ran',
      object: 'bad command',
      success: false,
    }),
    assistant(3, 'retried'),
  ], { deferTrailingToolSummary: true });

  assert.equal(projected[1].kind, 'activity_summary');
  assert.doesNotMatch(projected[1].title, /失败|错误|failed|error/i);
  assert.equal(projected[1].collapsedItems[0].tool.success, false);
  assert.match(projected[1].collapsedItems[0].tool.output, /stderr details/);
});

run('完成态最后一项是 assistant 文本时保留可展开处理摘要和最终文本', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    assistant(2, 'I will inspect'),
    tool(3, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    assistant(4, 'Final visible answer'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'processed');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
  assert.equal(projected[2].content, 'Final visible answer');
});

run('同一 raw history 多次投影得到相同可见结构', () => {
  const history = [
    user(1),
    tool(2, { verb: 'Created', object: 'a.txt', name: 'file_write' }),
    assistant(3, 'final'),
    taskComplete(4, 'done'),
  ];
  const reloaded = JSON.parse(JSON.stringify(history));

  const first = projectCollapsedTranscriptItems(history);
  const second = projectCollapsedTranscriptItems(reloaded);

  assert.deepEqual(
    first.map((item) => [item.kind, item.mode || item.role || item.tool?.tool, item.title || item.content || '']),
    second.map((item) => [item.kind, item.mode || item.role || item.tool?.tool, item.title || item.content || '']),
  );
});
