import assert from 'node:assert/strict';
import {
  pendingQuestionSessionIds,
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
