import assert from 'node:assert/strict';
import {
  addPendingQuestionRequest,
  pendingQuestionSessionIds,
  removePendingQuestionRequest,
  sessionHasPendingQuestion,
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

run('添加 question_request 时按 request_id 去重', () => {
  const first = [{ request_id: 'q1', session_id: 's1' }];
  const duplicate = addPendingQuestionRequest(first, { request_id: 'q1', session_id: 's1' });
  assert.equal(duplicate, first);

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
