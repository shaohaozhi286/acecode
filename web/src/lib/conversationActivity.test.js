import assert from 'node:assert/strict';
import {
  CONVERSATION_ACTIVITY_KIND,
  selectConversationActivity,
} from './conversationActivity.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('权限优先于问题、前台进度和后台任务', () => {
  const state = selectConversationActivity({
    foregroundBusy: true,
    foregroundActivity: { phase: 'tool_running', label: '运行工具' },
    permissionRequests: [{ request_id: 'p1', status: 'pending', has_request: true }],
    questionRequest: { request_id: 'q1', status: 'pending', has_request: true },
    subagentTasks: [{ id: 'child-1', status: 'running' }],
  });
  assert.equal(state.kind, CONVERSATION_ACTIVITY_KIND.PERMISSION);
  assert.equal(state.label, '等待权限确认');
  assert.equal(state.backgroundCount, 1);
});

run('问题优先于交互恢复和前台进度', () => {
  const state = selectConversationActivity({
    foregroundBusy: true,
    foregroundActivity: { phase: 'question_waiting', startedAtMs: 10 },
    questionRequest: { request_id: 'q1', status: 'pending', has_request: true },
  });
  assert.equal(state.kind, CONVERSATION_ACTIVITY_KIND.QUESTION);
  assert.equal(state.label, '等待你回答');
  assert.equal(state.needsAction, true);
});

run('等待阶段缺失 actionable payload 时显示恢复而非可操作文案', () => {
  const permission = selectConversationActivity({
    foregroundBusy: true,
    foregroundActivity: { phase: 'permission_waiting', startedAtMs: 10 },
  });
  assert.equal(permission.kind, CONVERSATION_ACTIVITY_KIND.RECOVERY);
  assert.equal(permission.label, '正在恢复权限请求');
  assert.equal(permission.needsAction, false);

  const question = selectConversationActivity({
    foregroundBusy: true,
    foregroundActivity: { phase: 'question_waiting', startedAtMs: 20 },
  });
  assert.equal(question.label, '正在恢复提问请求');
});

run('前台忙碌不依赖 transcript 工具块是否存在或过期', () => {
  const input = {
    foregroundBusy: true,
    foregroundActivity: {
      phase: 'model_waiting',
      label: '正在等待模型响应',
      startedAtMs: 100,
    },
  };
  const withoutTool = selectConversationActivity({ ...input, transcriptHasActiveTool: false });
  const withStaleTool = selectConversationActivity({ ...input, transcriptHasActiveTool: true });
  assert.deepEqual(withStaleTool, withoutTool);
  assert.equal(withoutTool.kind, CONVERSATION_ACTIVITY_KIND.FOREGROUND);
});

run('父会话 idle 且 child 运行时只显示后台状态', () => {
  const state = selectConversationActivity({
    foregroundBusy: false,
    subagentTasks: [
      { id: 'child-1', status: 'running', createdAtMs: 200 },
      { id: 'child-2', status: 'running', createdAtMs: 100 },
      { id: 'child-3', status: 'completed', createdAtMs: 50 },
    ],
  });
  assert.equal(state.kind, CONVERSATION_ACTIVITY_KIND.BACKGROUND);
  assert.equal(state.label, '2 个后台任务正在运行');
  assert.equal(state.startedAtMs, 100);
  assert.equal(state.detail, '主会话仍可继续输入');
});

run('前台和后台重叠时保留前台主状态与后台计数', () => {
  const state = selectConversationActivity({
    foregroundBusy: true,
    foregroundActivity: { phase: 'reasoning', label: '正在分析' },
    subagentTasks: [{ id: 'child-1', status: 'running' }],
  });
  assert.equal(state.kind, CONVERSATION_ACTIVITY_KIND.FOREGROUND);
  assert.equal(state.label, '正在分析');
  assert.equal(state.backgroundCount, 1);
  assert.equal(state.backgroundLabel, '1 个后台任务正在运行');
});

run('所有来源结束后返回 idle', () => {
  const state = selectConversationActivity({
    foregroundBusy: false,
    permissionRequests: [{ request_id: 'p1', status: 'resolved', has_request: true }],
    questionRequest: { request_id: 'q1', status: 'resolved', has_request: true },
    subagentTasks: [{ id: 'child-1', status: 'completed' }],
  });
  assert.equal(state.kind, CONVERSATION_ACTIVITY_KIND.IDLE);
});
