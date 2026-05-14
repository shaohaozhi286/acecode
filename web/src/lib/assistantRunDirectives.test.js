// 单元测试:buildAssistantRunDirectives 纯逻辑。
//
// 覆盖触发场景:
// - 连续 assistant + 中间穿插 tool 行: 只第一条带 header, 其余 continuation
// - 用户消息把 run 切断: 之后第一条 assistant 重新显示 header
// - 空内容 + 非 streaming 的 assistant: 隐藏整行 + 不消耗 header 名额
// - 空内容 + streaming 的 assistant: 正常显示 + 计入 run
// - 非数组 / null 输入: 返回空 Map

import assert from 'node:assert/strict';
import { buildAssistantRunDirectives } from './assistantRunDirectives.js';

function run(name, fn) {
  try {
    fn();
    console.log('ok - ' + name);
  } catch (err) {
    console.error('not ok - ' + name);
    throw err;
  }
}

run('非数组输入返回空 Map', () => {
  assert.equal(buildAssistantRunDirectives(null).size, 0);
  assert.equal(buildAssistantRunDirectives(undefined).size, 0);
  assert.equal(buildAssistantRunDirectives({}).size, 0);
});

run('单条 assistant 消息显示 header', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: 'hello' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: true });
});

run('连续 assistant + 中间 tool 行: 只第一条带 header', () => {
  // 模拟截图场景: assistant(空文本但 streaming 完成?) → tool → assistant → tool → assistant
  // 这里所有 assistant 都有内容, 只关心 header 合并
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '思考中...' },
    { kind: 'tool', id: 2, tool: { isDone: true } },
    { kind: 'msg', id: 3, role: 'assistant', content: '继续' },
    { kind: 'tool', id: 4, tool: { isDone: true } },
    { kind: 'msg', id: 5, role: 'assistant', content: '完成' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(3), { showHeader: false, showFooter: false });
  assert.deepEqual(d.get(5), { showHeader: false, showFooter: true });
});

run('user 消息切断 run, 之后第一条 assistant 重新带 header', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '答一' },
    { kind: 'msg', id: 2, role: 'assistant', content: '答一-续' },
    { kind: 'msg', id: 3, role: 'user', content: '再问' },
    { kind: 'msg', id: 4, role: 'assistant', content: '答二' },
    { kind: 'msg', id: 5, role: 'assistant', content: '答二-续' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(2), { showHeader: false, showFooter: true });
  assert.deepEqual(d.get(4), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(5), { showHeader: false, showFooter: true });
});

run('空内容 + 非 streaming 的 assistant 隐藏 + 不消耗 header 名额', () => {
  // 触发场景: LLM 仅发起 tool_call 不发文本, daemon 仍落库一条空 assistant
  // 期望: 该条 hide, 头部名额留给后续真正有内容的 assistant
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '   ' },
    { kind: 'tool', id: 2, tool: { isDone: true } },
    { kind: 'msg', id: 3, role: 'assistant', content: '真正的输出' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { hide: true });
  assert.deepEqual(d.get(3), { showHeader: true, showFooter: true });
});

run('空内容 + streaming 的 assistant 不隐藏 + 计入 run', () => {
  // 触发场景: 流式输出刚开始, 内容暂时为空, 不能隐藏
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '', streaming: true },
    { kind: 'msg', id: 2, role: 'assistant', content: '后续' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(2), { showHeader: false, showFooter: true });
});

run('多个 user 之间的多个 run, 各自独立计算', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'user', content: 'q1' },
    { kind: 'msg', id: 2, role: 'assistant', content: 'a1' },
    { kind: 'msg', id: 3, role: 'assistant', content: 'a1-cont' },
    { kind: 'msg', id: 4, role: 'user', content: 'q2' },
    { kind: 'msg', id: 5, role: 'assistant', content: 'a2' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(2), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(3), { showHeader: false, showFooter: true });
  assert.deepEqual(d.get(5), { showHeader: true, showFooter: true });
});

run('system 行不影响 run', () => {
  // 触发场景: 系统插入一条警告(role 既不是 user 也不是 assistant), 不应重置 run
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: 'a1' },
    { kind: 'msg', id: 2, role: 'system', content: '警告' },
    { kind: 'msg', id: 3, role: 'assistant', content: 'a2' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(3), { showHeader: false, showFooter: true });
});

run('activity summary 和 completion summary 不切断 footer run', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: 'first' },
    { kind: 'activity_summary', id: 's1', title: '调用 2 个工具' },
    { kind: 'msg', id: 2, role: 'assistant', content: 'second' },
    { kind: 'completion_summary', id: 'c1', title: '总结：done' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(2), { showHeader: false, showFooter: true });
});
