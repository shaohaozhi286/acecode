// AskUserQuestion picker 的纯逻辑 helper。
// 保持与 daemon question_answer 协议兼容,供 React 组件与 Node 单测复用。

function toText(value, fallback = '') {
  if (typeof value === 'string') return value;
  if (value == null) return fallback;
  return String(value);
}

function optionValue(option, index) {
  const label = toText(option?.label, `Option ${index + 1}`);
  const value = option && option.value != null ? toText(option.value, label) : label;
  return { label, value };
}

export function normalizeQuestionRequest(request = {}) {
  const rawQuestions = Array.isArray(request.questions) ? request.questions : [];
  return {
    requestId: toText(request.request_id),
    sessionId: toText(request.session_id),
    questions: rawQuestions.map((q, qi) => {
      const text = toText(q?.text || q?.question || q?.header, `Question ${qi + 1}`);
      const options = Array.isArray(q?.options) ? q.options : [];
      return {
        id: toText(q?.id || q?.question || q?.text, text),
        text,
        header: toText(q?.header),
        multiSelect: !!q?.multiSelect,
        options: options.map((opt, oi) => {
          const normalized = optionValue(opt, oi);
          return {
            ...normalized,
            description: toText(opt?.description),
          };
        }),
      };
    }),
  };
}

export function makeInitialAnswers(questions = []) {
  return questions.map(() => ({ selected: [], custom: '' }));
}

export function isQuestionAnswered(answer = {}) {
  return (Array.isArray(answer.selected) && answer.selected.length > 0) ||
    toText(answer.custom).trim().length > 0;
}

export function allQuestionsAnswered(questions = [], answers = []) {
  return questions.length > 0 && questions.every((_, index) => isQuestionAnswered(answers[index]));
}

export function getNavigationState(currentIndex, questions = [], answers = []) {
  const total = questions.length;
  const currentAnswered = isQuestionAnswered(answers[currentIndex]);
  const isLast = total > 0 && currentIndex >= total - 1;
  return {
    total,
    current: total === 0 ? 0 : currentIndex + 1,
    isLast,
    currentAnswered,
    allAnswered: allQuestionsAnswered(questions, answers),
    canGoPrev: currentIndex > 0,
    canGoNext: total > 0 && !isLast && currentAnswered,
    canSubmit: total > 0 && isLast && allQuestionsAnswered(questions, answers),
  };
}

export function toggleAnswerSelection(answer = {}, value, multiSelect) {
  const selected = Array.isArray(answer.selected) ? answer.selected : [];
  const textValue = toText(value);
  if (!textValue) return { ...answer, selected };
  if (!multiSelect) return { ...answer, selected: [textValue] };
  const hasValue = selected.includes(textValue);
  return {
    ...answer,
    selected: hasValue
      ? selected.filter((item) => item !== textValue)
      : [...selected, textValue],
  };
}

export function setAnswerCustom(answer = {}, custom) {
  return { ...answer, custom: toText(custom) };
}

export function buildQuestionAnswerPayload(request = {}, questions = [], answers = []) {
  const payload = {
    request_id: toText(request.requestId || request.request_id),
    session_id: toText(request.sessionId || request.session_id),
    answers: questions.map((q, index) => {
      const answer = answers[index] || {};
      const selected = Array.isArray(answer.selected)
        ? answer.selected.map((item) => toText(item)).filter(Boolean)
        : [];
      const out = {
        question_id: toText(q.id || q.question || q.text),
        selected,
      };
      const custom = toText(answer.custom).trim();
      if (custom) out.custom_text = custom;
      return out;
    }),
  };
  if (!payload.session_id) delete payload.session_id;
  return payload;
}

export function buildQuestionCancelPayload(request = {}) {
  const payload = {
    request_id: toText(request.requestId || request.request_id),
    session_id: toText(request.sessionId || request.session_id),
    cancelled: true,
  };
  if (!payload.session_id) delete payload.session_id;
  return payload;
}
