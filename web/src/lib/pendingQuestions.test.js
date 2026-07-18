import assert from 'node:assert/strict';
import {
  QUESTION_REQUEST_STATUS,
  addPendingQuestionRequest,
  clearResolvedQuestionRequests,
  closePendingQuestionRequest,
  pendingQuestionSessionIds,
  questionOriginLabel,
  removePendingQuestionRequest,
  sessionHasPendingQuestion,
  visibleQuestionRequest,
} from './pendingQuestions.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('从 question_request 队列派生待回复 session ids', () => {
  const ids = pendingQuestionSessionIds([
    { request_id: 'q1', session_id: 's1' },
    { request_id: 'q1', session_id: 's1' },
    { request_id: 'q2', session_id: 's2' },
    { session_id: 'missing-request-id' },
  ]);
  assert.deepEqual([...ids].sort(), ['s1', 's2']);
});

run('添加 question_request 时按 request_id 合并且不重复排队', () => {
  const first = [{ request_id: 'q1', session_id: 's1' }];
  const duplicate = addPendingQuestionRequest(first, { request_id: 'q1', session_id: 's1' });
  assert.deepEqual(duplicate.map((item) => item.request_id), ['q1']);
  assert.equal(duplicate[0].status, QUESTION_REQUEST_STATUS.PENDING);

  const added = addPendingQuestionRequest(first, { request_id: 'q2', session_id: 's1' });
  assert.deepEqual(added.map((item) => item.request_id), ['q1', 'q2']);
});

run('question_closed 按 request_id 清理待回复队列', () => {
  const requests = [
    { request_id: 'q1', session_id: 's1' },
    { request_id: 'q2', session_id: 's1' },
  ];
  const next = removePendingQuestionRequest(requests, 'q1');
  assert.deepEqual(next.map((item) => item.request_id), ['q2']);
});

run('未知 question_closed 不改变待回复队列引用', () => {
  const requests = [{ request_id: 'q1', session_id: 's1' }];
  const next = removePendingQuestionRequest(requests, 'missing');
  assert.equal(next, requests);
});

run('先到达的 question_closed 留下 tombstone 并阻止旧快照复活', () => {
  const closed = closePendingQuestionRequest([], {
    request_id: 'q1',
    session_id: 'child-1',
  }, { ownerSessionId: 'parent-1' });
  assert.equal(closed[0].status, QUESTION_REQUEST_STATUS.RESOLVED);
  assert.equal(closed[0].has_request, false);

  const replayed = addPendingQuestionRequest(closed, {
    request_id: 'q1',
    session_id: 'child-1',
    questions: [{ question: '继续吗?' }],
  }, { ownerSessionId: 'parent-1' });
  assert.equal(replayed[0].status, QUESTION_REQUEST_STATUS.RESOLVED);
  assert.equal(replayed[0].has_request, true);
  assert.equal(visibleQuestionRequest(replayed, 'parent-1', {
    owners: { 'child-1': 'parent-1' },
  }), null);
  assert.deepEqual([...pendingQuestionSessionIds(replayed, '', {
    owners: { 'child-1': 'parent-1' },
  })], []);
});

run('turn 结束后只清除对应 session 的已解决 tombstone', () => {
  const requests = [
    closePendingQuestionRequest([], { request_id: 'q1', session_id: 'child-1' })[0],
    closePendingQuestionRequest([], { request_id: 'q2', session_id: 'child-2' })[0],
    addPendingQuestionRequest([], { request_id: 'q3', session_id: 'child-1' })[0],
  ];
  const next = clearResolvedQuestionRequests(requests, 'child-1');
  assert.deepEqual(next.map((item) => item.request_id), ['q2', 'q3']);
});

run('后台子会话问题归属父会话并显示来源', () => {
  const ownership = {
    parentId: 'parent-1',
    owners: { 'child-1': 'parent-1' },
    titles: { 'child-1': '检查构建' },
  };
  const requests = addPendingQuestionRequest([], {
    request_id: 'q1',
    session_id: 'child-1',
  }, { ownerSessionId: 'parent-1' });
  assert.equal(visibleQuestionRequest(requests, 'parent-1', ownership)?.request_id, 'q1');
  assert.deepEqual([...pendingQuestionSessionIds(requests, '', ownership)], ['parent-1']);
  assert.equal(questionOriginLabel(requests[0], ownership), '来自后台任务:检查构建');
});

run('缺少 session_id 的 question_request 回退到当前 active session', () => {
  const ids = pendingQuestionSessionIds([
    { request_id: 'q1' },
  ], 'active-session');
  assert.deepEqual([...ids], ['active-session']);
});

run('session id 别名可命中待回复状态', () => {
  const ids = new Set(['s1', 's2']);
  assert.equal(sessionHasPendingQuestion({ id: 's1' }, ids), true);
  assert.equal(sessionHasPendingQuestion({ session_id: 's2' }, ids), true);
  assert.equal(sessionHasPendingQuestion({ sessionId: 's3' }, ids), false);
});
