// AskUserQuestion 内联 picker:停靠在输入框上方,不使用全屏 modal。
// 支持单选 / 多选 / 自定义答案 / 多题分页 / 键盘操作。

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { connection } from '../lib/connection.js';
import { clsx } from '../lib/format.js';
import { VsIcon } from './Icon.jsx';
import {
  buildQuestionAnswerPayload,
  buildQuestionCancelPayload,
  getNavigationState,
  makeInitialAnswers,
  normalizeQuestionRequest,
  setAnswerCustom,
  toggleAnswerSelection,
} from '../lib/questionPicker.js';

function focusSoon(ref) {
  requestAnimationFrame(() => ref.current?.focus());
}

export function QuestionPicker({ request, onResolve }) {
  const normalized = useMemo(() => normalizeQuestionRequest(request), [request]);
  const { questions } = normalized;
  const [answers, setAnswers] = useState(() => makeInitialAnswers(questions));
  const [currentIndex, setCurrentIndex] = useState(0);
  const [focusIndex, setFocusIndex] = useState(0);
  const [collapsed, setCollapsed] = useState(false);
  const rootRef = useRef(null);
  const customRef = useRef(null);

  useEffect(() => {
    setAnswers(makeInitialAnswers(questions));
    setCurrentIndex(0);
    setFocusIndex(0);
    setCollapsed(false);
    focusSoon(rootRef);
  }, [normalized.requestId, questions]);

  const question = questions[currentIndex];
  const answer = answers[currentIndex] || { selected: [], custom: '' };
  const optionCount = question?.options?.length || 0;
  const customIndex = optionCount;
  const nav = getNavigationState(currentIndex, questions, answers);

  const updateAnswer = useCallback((index, updater) => {
    setAnswers((prev) => prev.map((item, i) => i === index ? updater(item) : item));
  }, []);

  const resolve = useCallback(() => {
    onResolve?.();
  }, [onResolve]);

  const cancel = useCallback(() => {
    connection.sendQuestionAnswer(buildQuestionCancelPayload(normalized));
    resolve();
  }, [normalized, resolve]);

  const submit = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (!state.canSubmit) return;
    connection.sendQuestionAnswer(buildQuestionAnswerPayload(normalized, questions, answers));
    resolve();
  }, [answers, currentIndex, normalized, questions, resolve]);

  const goPrev = useCallback(() => {
    setCurrentIndex((value) => Math.max(0, value - 1));
    setFocusIndex(0);
    focusSoon(rootRef);
  }, []);

  const goNext = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (!state.canGoNext) return;
    setCurrentIndex((value) => Math.min(questions.length - 1, value + 1));
    setFocusIndex(0);
    focusSoon(rootRef);
  }, [answers, currentIndex, questions]);

  const primaryAction = useCallback(() => {
    const state = getNavigationState(currentIndex, questions, answers);
    if (state.isLast) submit();
    else goNext();
  }, [answers, currentIndex, goNext, questions, submit]);

  const selectOption = useCallback((optionIndex) => {
    const opt = question?.options?.[optionIndex];
    if (!opt) return;
    setFocusIndex(optionIndex);
    updateAnswer(currentIndex, (item) => toggleAnswerSelection(item, opt.value, !!question.multiSelect));
  }, [currentIndex, question, updateAnswer]);

  const setCustom = useCallback((value) => {
    updateAnswer(currentIndex, (item) => setAnswerCustom(item, value));
  }, [currentIndex, updateAnswer]);

  const moveFocus = useCallback((delta) => {
    const count = optionCount + 1;
    setFocusIndex((value) => Math.min(count - 1, Math.max(0, value + delta)));
  }, [optionCount]);

  const onKeyDown = useCallback((event) => {
    if (!question) return;
    const target = event.target;
    const tag = target?.tagName;
    const inTextInput = tag === 'INPUT' || tag === 'TEXTAREA';

    if (event.key === 'Escape') {
      event.preventDefault();
      cancel();
      return;
    }

    if (inTextInput) {
      if (event.key === 'Enter') {
        event.preventDefault();
        primaryAction();
      }
      return;
    }

    if (/^[1-9]$/.test(event.key)) {
      const index = Number(event.key) - 1;
      if (index < optionCount) {
        event.preventDefault();
        selectOption(index);
      }
      return;
    }

    if (event.key === 'ArrowDown') {
      event.preventDefault();
      moveFocus(1);
      return;
    }
    if (event.key === 'ArrowUp') {
      event.preventDefault();
      moveFocus(-1);
      return;
    }
    if (event.key === ' ') {
      event.preventDefault();
      if (focusIndex < optionCount) selectOption(focusIndex);
      else customRef.current?.focus();
      return;
    }
    if (event.key === 'Enter') {
      event.preventDefault();
      if (nav.currentAnswered) {
        primaryAction();
      } else if (focusIndex < optionCount) {
        selectOption(focusIndex);
      } else {
        customRef.current?.focus();
      }
    }
  }, [cancel, focusIndex, moveFocus, nav.currentAnswered, optionCount, primaryAction, question, selectOption]);

  if (!question) return null;

  return (
    <section
      ref={rootRef}
      tabIndex={-1}
      onKeyDown={onKeyDown}
      aria-label="AskUserQuestion"
      className="mx-2.5 mb-2 shrink-0 rounded-xl border border-border bg-surface ace-shadow-lg outline-none overflow-hidden"
    >
      <div className="min-h-10 px-3 py-2 border-b border-border bg-surface-alt flex items-center gap-2">
        <div className="min-w-0 flex-1">
          {question.header && (
            <div className="text-[10px] uppercase tracking-wide text-accent font-semibold truncate">
              {question.header}
            </div>
          )}
          <div className="text-[13px] font-semibold text-fg truncate">
            {question.text}
          </div>
        </div>
        <button
          type="button"
          onClick={() => setCollapsed((value) => !value)}
          className="w-7 h-7 rounded-md flex items-center justify-center text-fg-2 hover:bg-surface-hi transition"
          title={collapsed ? '展开' : '折叠'}
        >
          <VsIcon name={collapsed ? 'expandRight' : 'expandDown'} size={14} />
        </button>
        <button
          type="button"
          onClick={cancel}
          className="w-7 h-7 rounded-md flex items-center justify-center text-fg-2 hover:bg-danger-bg hover:text-danger transition"
          title="取消回答"
        >
          <VsIcon name="close" size={14} />
        </button>
      </div>

      {collapsed ? (
        <div className="px-3 py-2 text-[12px] text-fg-2 flex items-center justify-between gap-3">
          <span className="truncate">已折叠,继续等待回答。</span>
          <span className="shrink-0 text-fg-mute">{nav.current}/{nav.total}</span>
        </div>
      ) : (
        <>
          <div className="p-2.5 max-h-[42vh] overflow-y-auto flex flex-col gap-1.5">
            {question.options.map((opt, index) => {
              const selected = answer.selected?.includes(opt.value);
              const focused = focusIndex === index;
              return (
                <button
                  key={`${opt.value}-${index}`}
                  type="button"
                  onClick={() => selectOption(index)}
                  onFocus={() => setFocusIndex(index)}
                  aria-pressed={selected}
                  className={clsx(
                    'w-full text-left rounded-lg border px-2.5 py-2 flex items-center gap-2 transition outline-none',
                    selected
                      ? 'bg-accent-bg border-accent text-fg'
                      : 'bg-surface-alt border-border hover:bg-surface-hi',
                    focused && 'ring-2 ring-accent/20 border-accent',
                  )}
                >
                  <span className="w-5 shrink-0 text-[12px] font-semibold text-fg-mute tabular-nums">
                    {index + 1}
                  </span>
                  <span className="min-w-0 flex-1">
                    <span className="block text-[12px] font-medium text-fg break-words">{opt.label}</span>
                    {opt.description && (
                      <span className="block mt-0.5 text-[11px] leading-[15px] text-fg-mute break-words">
                        {opt.description}
                      </span>
                    )}
                  </span>
                  <span className="w-5 h-5 shrink-0 flex items-center justify-center">
                    {selected ? (
                      <VsIcon name="ok" size={14} mono={false} />
                    ) : (
                      <span className="w-3.5 h-3.5 rounded-full border border-border" />
                    )}
                  </span>
                </button>
              );
            })}

            <label
              className={clsx(
                'rounded-lg border px-2.5 py-2 flex items-center gap-2 transition',
                answer.custom?.trim()
                  ? 'bg-accent-bg border-accent'
                  : 'bg-surface-alt border-border',
                focusIndex === customIndex && 'ring-2 ring-accent/20 border-accent',
              )}
            >
              <span className="w-5 shrink-0 text-[12px] font-semibold text-fg-mute tabular-nums">
                {customIndex + 1}
              </span>
              <span className="min-w-0 flex-1 flex flex-col gap-1">
                <span className="text-[12px] font-medium text-fg">其他</span>
                <input
                  ref={customRef}
                  type="text"
                  value={answer.custom || ''}
                  onFocus={() => setFocusIndex(customIndex)}
                  onChange={(event) => setCustom(event.target.value)}
                  placeholder="输入自定义答案"
                  className="h-7 w-full rounded-md border border-border bg-surface px-2 text-[12px] text-fg outline-none focus:border-accent"
                />
              </span>
              <span className="w-5 h-5 shrink-0 flex items-center justify-center">
                {answer.custom?.trim() ? (
                  <VsIcon name="ok" size={14} mono={false} />
                ) : (
                  <span className="w-3.5 h-3.5 rounded-full border border-border" />
                )}
              </span>
            </label>
          </div>

          <div className="px-3 py-2 border-t border-border bg-surface-alt flex items-center gap-2">
            <button
              type="button"
              onClick={goPrev}
              disabled={!nav.canGoPrev}
              className="w-7 h-7 rounded-md border border-border bg-surface text-[13px] text-fg-2 disabled:opacity-40 disabled:cursor-not-allowed hover:bg-surface-hi transition"
              title="上一题"
            >
              &lt;
            </button>
            <div className="text-[12px] text-fg-mute tabular-nums min-w-10 text-center">
              {nav.current}/{nav.total}
            </div>
            <button
              type="button"
              onClick={goNext}
              disabled={!nav.canGoNext}
              className="w-7 h-7 rounded-md border border-border bg-surface text-[13px] text-fg-2 disabled:opacity-40 disabled:cursor-not-allowed hover:bg-surface-hi transition"
              title="下一题"
            >
              &gt;
            </button>
            <div className="flex-1" />
            <span className="hidden sm:inline text-[11px] text-fg-mute">
              数字选择 · Enter 确认 · Esc 取消
            </span>
            <button
              type="button"
              onClick={primaryAction}
              disabled={nav.isLast ? !nav.canSubmit : !nav.canGoNext}
              className="px-3 h-7 rounded-md bg-accent text-white text-[12px] font-medium disabled:opacity-50 disabled:cursor-not-allowed hover:opacity-90 transition"
            >
              {nav.isLast ? '提交' : '下一题'}
            </button>
          </div>
        </>
      )}
    </section>
  );
}
