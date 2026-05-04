// QuestionPicker helper 单元测试:覆盖 payload、取消和导航禁用等纯逻辑。

import assert from 'node:assert/strict';
import {
  allQuestionsAnswered,
  buildQuestionAnswerPayload,
  buildQuestionCancelPayload,
  getNavigationState,
  isQuestionAnswered,
  makeInitialAnswers,
  normalizeQuestionRequest,
  toggleAnswerSelection,
  setAnswerCustom,
} from './questionPicker.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const request = normalizeQuestionRequest({
  request_id: 'req-1',
  session_id: 'sid-1',
  questions: [
    {
      id: 'q1',
      text: '你想做什么?',
      options: [
        { label: '修复 bug', value: 'fix-bug', description: '诊断问题' },
        { label: '添加功能', value: 'add-feature', description: '实现新能力' },
      ],
    },
    {
      id: 'q2',
      text: '需要哪些质量项?',
      multiSelect: true,
      options: [
        { label: '测试', value: 'tests' },
        { label: '文档', value: 'docs' },
      ],
    },
  ],
});

run('单选 payload 使用 option value', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleAnswerSelection(answers[0], 'fix-bug', false);
  answers[1] = toggleAnswerSelection(answers[1], 'tests', true);
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[0], { question_id: 'q1', selected: ['fix-bug'] });
});

run('多选 payload 保留多个选中值', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = toggleAnswerSelection(answers[0], 'add-feature', false);
  answers[1] = toggleAnswerSelection(answers[1], 'tests', true);
  answers[1] = toggleAnswerSelection(answers[1], 'docs', true);
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[1], { question_id: 'q2', selected: ['tests', 'docs'] });
});

run('自定义答案 payload 写入 custom_text', () => {
  const answers = makeInitialAnswers(request.questions);
  answers[0] = setAnswerCustom(answers[0], '  其它需求  ');
  answers[1] = toggleAnswerSelection(answers[1], 'docs', true);
  const payload = buildQuestionAnswerPayload(request, request.questions, answers);
  assert.deepEqual(payload.answers[0], {
    question_id: 'q1',
    selected: [],
    custom_text: '其它需求',
  });
});

run('取消 payload 不包含部分答案', () => {
  assert.deepEqual(buildQuestionCancelPayload(request), {
    request_id: 'req-1',
    session_id: 'sid-1',
    cancelled: true,
  });
});

run('未回答问题禁用推进和提交', () => {
  const answers = makeInitialAnswers(request.questions);
  const state = getNavigationState(0, request.questions, answers);
  assert.equal(isQuestionAnswered(answers[0]), false);
  assert.equal(allQuestionsAnswered(request.questions, answers), false);
  assert.equal(state.canGoNext, false);
  assert.equal(state.canSubmit, false);
});
