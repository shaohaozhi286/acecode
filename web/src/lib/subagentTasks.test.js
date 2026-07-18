// subagentTasks.js 的单元测试。
//
// 后台任务面板的全部状态转移都在纯函数层:REST 快照归一化、refetch 合并
// (保留实时聚合字段)、WS 事件增量、本地中止标记、分组/徽标计数、
// 卡片显示格式化。DOM 端(SubagentPanel.jsx)只做结构→className 映射。
//
// 覆盖:
//  - normalizeSubagentTask:busy/status 判定、tokens 提取、坏输入容错
//  - mergeSubagentTaskList:refetch 不清零 toolCount、保留 aborted 标记、排序
//  - applySubagentSessionEvent:busy_changed 状态迁移、usage 单调递增、
//    tool_start 计数、无关事件返回原引用
//  - markSubagentTaskAborted 后 busy_changed(false) 不把状态改回 completed
//  - 分组 / runningCount / 格式化函数边界

import assert from 'node:assert/strict';
import {
  SUBAGENT_TASK_STATUS,
  applySubagentSessionEvent,
  formatElapsed,
  formatTaskTokens,
  markSubagentTaskAborted,
  mergeSubagentTaskList,
  normalizeSubagentTask,
  removeSubagentTask,
  runningSubagentCount,
  shouldRefreshSubagentTasksFromStatus,
  subagentTaskGroups,
  taskDisplayTitle,
  taskElapsedSeconds,
  taskStatsParts,
} from './subagentTasks.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function makeSession(overrides = {}) {
  return {
    id: 'child-1',
    parent_session_id: 'parent-1',
    title: '读取 test 文件夹内容',
    summary: 'list the folder',
    busy: false,
    status: 'idle',
    created_at: '2026-07-05T09:00:00Z',
    updated_at: '2026-07-05T09:00:39Z',
    turn_count: 1,
    session_token_usage: { total_tokens: 34000 },
    ...overrides,
  };
}

run('normalizeSubagentTask 提取快照字段,busy=false → completed', () => {
  const task = normalizeSubagentTask(makeSession());
  assert.equal(task.id, 'child-1');
  assert.equal(task.parentId, 'parent-1');
  assert.equal(task.status, SUBAGENT_TASK_STATUS.COMPLETED);
  assert.equal(task.tokens, 34000);
  assert.equal(task.turnCount, 1);
  assert.equal(task.toolCount, 0);
});

run('normalizeSubagentTask busy=true 或 status=running → running', () => {
  assert.equal(normalizeSubagentTask(makeSession({ busy: true })).status,
               SUBAGENT_TASK_STATUS.RUNNING);
  assert.equal(normalizeSubagentTask(makeSession({ status: 'running' })).status,
               SUBAGENT_TASK_STATUS.RUNNING);
});

run('normalizeSubagentTask 坏输入返回 null,不抛异常', () => {
  assert.equal(normalizeSubagentTask(null), null);
  assert.equal(normalizeSubagentTask({}), null);
  assert.equal(normalizeSubagentTask('x'), null);
});

run('mergeSubagentTaskList refetch 保留实时聚合字段与 aborted 标记', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'tool_start', session_id: 'child-1', payload: { tool: 'bash' },
  });
  tasks = markSubagentTaskAborted(tasks, 'child-1');
  // refetch:同一会话的新快照(busy=false)进来,不应清零 toolCount,
  // 也不应把「已中止」洗回 completed。
  const merged = mergeSubagentTaskList(tasks, [makeSession({ busy: false })]);
  assert.equal(merged[0].toolCount, 1);
  assert.equal(merged[0].lastTool, 'bash');
  assert.equal(merged[0].status, SUBAGENT_TASK_STATUS.ABORTED);
});

run('mergeSubagentTaskList 按创建时间倒序(新任务在前)', () => {
  const merged = mergeSubagentTaskList([], [
    makeSession({ id: 'old', created_at: '2026-07-05T08:00:00Z' }),
    makeSession({ id: 'new', created_at: '2026-07-05T10:00:00Z' }),
  ]);
  assert.deepEqual(merged.map((t) => t.id), ['new', 'old']);
});

run('applySubagentSessionEvent busy_changed 完成迁移 + usage 单调更新', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'usage', session_id: 'child-1', payload: { total_tokens: 40000 },
  });
  assert.equal(tasks[0].tokens, 40000);
  // 更小的 usage(乱序旧帧)不回退。
  const same = applySubagentSessionEvent(tasks, {
    type: 'usage', session_id: 'child-1', payload: { total_tokens: 100 },
  });
  assert.equal(same[0].tokens, 40000);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'busy_changed', session_id: 'child-1', payload: { busy: false },
  });
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.COMPLETED);
});

run('session_status 只为明确匹配当前 parent 的未知 child 触发刷新', () => {
  const known = new Set(['child-known']);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    session_id: 'child-new',
    parent_session_id: 'parent-1',
    payload: {
      session_id: 'child-new',
      parent_session_id: 'parent-1',
      busy: true,
    },
  }), true);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    payload: {
      session_id: 'child-new',
      parent_session_id: 'parent-2',
      busy: true,
    },
  }), false);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    payload: { session_id: 'unrelated', busy: true },
  }), false);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    payload: {
      session_id: 'child-known',
      parent_session_id: 'parent-1',
      busy: true,
    },
  }), false);
});

run('已知 child 的 session_status 可直接完成任务状态', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'session_status',
    payload: {
      session_id: 'child-1',
      parent_session_id: 'parent-1',
      busy: false,
    },
  });
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.COMPLETED);
});

run('applySubagentSessionEvent 无关会话/无关类型返回原数组引用', () => {
  const tasks = mergeSubagentTaskList([], [makeSession()]);
  assert.equal(applySubagentSessionEvent(tasks, {
    type: 'busy_changed', session_id: 'other', payload: { busy: true },
  }), tasks);
  assert.equal(applySubagentSessionEvent(tasks, {
    type: 'token', session_id: 'child-1', payload: { text: 'x' },
  }), tasks);
});

run('中止后 busy_changed(false) 不把「已中止」洗成 completed', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = markSubagentTaskAborted(tasks, 'child-1');
  tasks = applySubagentSessionEvent(tasks, {
    type: 'busy_changed', session_id: 'child-1', payload: { busy: false },
  });
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.ABORTED);
});

run('session_updated 更新卡片标题(auto-title 生成到达)', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ title: '' })]);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'session_updated', session_id: 'child-1', payload: { title: '新标题' },
  });
  assert.equal(tasks[0].title, '新标题');
});

run('分组与徽标计数', () => {
  const tasks = mergeSubagentTaskList([], [
    makeSession({ id: 'a', busy: true }),
    makeSession({ id: 'b', busy: false }),
    makeSession({ id: 'c', status: 'running' }),
  ]);
  const groups = subagentTaskGroups(tasks);
  assert.equal(groups.running.length, 2);
  assert.equal(groups.settled.length, 1);
  assert.equal(runningSubagentCount(tasks), 2);
});

run('removeSubagentTask 清除单个任务;未命中返回原引用', () => {
  const tasks = mergeSubagentTaskList([], [makeSession()]);
  assert.equal(removeSubagentTask(tasks, 'nope'), tasks);
  assert.equal(removeSubagentTask(tasks, 'child-1').length, 0);
});

run('formatTaskTokens / formatElapsed / taskStatsParts 边界', () => {
  assert.equal(formatTaskTokens(0), '0');
  assert.equal(formatTaskTokens(999), '999');
  assert.equal(formatTaskTokens(34000), '34.0k');
  assert.equal(formatTaskTokens(240000), '240k');
  assert.equal(formatElapsed(9), '09s');
  assert.equal(formatElapsed(83), '1m23s');
  assert.equal(formatElapsed(3700), '1h01m');
  const task = normalizeSubagentTask(makeSession());
  // 已结束任务耗时 = updated - created = 39s。
  assert.equal(taskElapsedSeconds(task), 39);
  assert.deepEqual(taskStatsParts({ tokens: 34000, toolCount: 1, lastTool: 'bash' }),
                   ['34.0k tokens', '1 tool', 'bash']);
  assert.deepEqual(taskStatsParts({ tokens: 0, toolCount: 0, lastTool: '' }), []);
});

run('taskDisplayTitle 标题 → 摘要 → id 逐级退化', () => {
  assert.equal(taskDisplayTitle({ title: 'T', summary: 'S', id: 'i' }), 'T');
  assert.equal(taskDisplayTitle({ title: '', summary: 'S', id: 'i' }), 'S');
  assert.equal(taskDisplayTitle({ title: '', summary: '', id: 'i' }), 'i');
  // auto-title 生成失败的 "[Error] ..." 占位对用户无意义,退到摘要。
  assert.equal(taskDisplayTitle({ title: '[Error] Failed to parse response JSON', summary: 'S', id: 'i' }), 'S');
});

console.log('subagentTasks.test.js: all tests passed');
