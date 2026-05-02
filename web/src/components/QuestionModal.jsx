// AskUserQuestion 弹框:1-4 questions × 2-4 options + 自动追加 "Other..." 输入框。
// multiSelect=false → radio,multiSelect=true → checkbox。Esc/取消按钮等于 cancelled。

import { useEffect, useState } from 'react';
import { connection } from '../lib/connection.js';
import { Modal } from './Modal.jsx';
import { clsx } from '../lib/format.js';

export function QuestionModal({ request, onResolve }) {
  const questions = request.questions || [];
  const [answers, setAnswers] = useState(() =>
    questions.map(() => ({ selected: [], custom: '' })));

  useEffect(() => {
    setAnswers(questions.map(() => ({ selected: [], custom: '' })));
  }, [request]);

  const setSelected = (qi, value, multi) => {
    setAnswers((prev) => prev.map((a, i) => {
      if (i !== qi) return a;
      const has = a.selected.includes(value);
      if (multi) {
        return { ...a, selected: has ? a.selected.filter((v) => v !== value) : [...a.selected, value] };
      }
      return { ...a, selected: [value] };
    }));
  };

  const setCustom = (qi, txt) => {
    setAnswers((prev) => prev.map((a, i) => i === qi ? { ...a, custom: txt } : a));
  };

  const ok = answers.length === questions.length &&
    answers.every((a) => a.selected.length > 0 || a.custom.trim().length > 0);

  const cancel = (close) => {
    connection.sendQuestionAnswer({ request_id: request.request_id, session_id: request.session_id, cancelled: true });
    close();
    setTimeout(() => onResolve?.(), 220);
  };

  const submit = (close) => {
    if (!ok) return;
    const payload = {
      request_id: request.request_id,
      session_id: request.session_id,
      answers: questions.map((q, i) => {
        const a = answers[i];
        const out = { question_id: q.id || q.question || '', selected: a.selected };
        if (a.custom.trim()) out.custom_text = a.custom.trim();
        return out;
      }),
    };
    connection.sendQuestionAnswer(payload);
    close();
    setTimeout(() => onResolve?.(), 220);
  };

  return (
    <Modal width={600} onClose={onResolve}>
      {({ close }) => (
        <>
          <div className="px-4.5 py-3 bg-accent-bg/50 border-b border-border flex items-center gap-2">
            <span className="text-base">❓</span>
            <h3 className="text-[14px] font-semibold">需要回答</h3>
          </div>
          <div className="px-4.5 py-4 max-h-[60vh] overflow-y-auto flex flex-col gap-4">
            {questions.map((q, qi) => {
              const multi = !!q.multiSelect;
              const a = answers[qi] || { selected: [], custom: '' };
              return (
                <div key={qi} className="flex flex-col gap-2">
                  <div className="text-[13px] font-semibold">{q.text || q.question}</div>
                  <div className="flex flex-col gap-1">
                    {(q.options || []).map((opt, oi) => {
                      const value = opt.value ?? opt.label;
                      const checked = a.selected.includes(value);
                      return (
                        <label
                          key={oi}
                          className={clsx(
                            'flex items-center gap-2 px-3 py-2 rounded-md border text-[12px] cursor-pointer transition',
                            checked
                              ? 'bg-accent-bg border-accent text-fg'
                              : 'bg-surface-alt border-border hover:bg-surface-hi',
                          )}
                        >
                          <input
                            type={multi ? 'checkbox' : 'radio'}
                            name={`q-${qi}`}
                            checked={checked}
                            onChange={() => setSelected(qi, value, multi)}
                            className="accent-accent"
                          />
                          <span className="flex-1">{opt.label}</span>
                        </label>
                      );
                    })}
                  </div>
                  <input
                    type="text"
                    placeholder="Other... (自定义答案)"
                    value={a.custom}
                    onChange={(e) => setCustom(qi, e.target.value)}
                    className="h-8 px-3 text-[12px] rounded-md border border-border bg-surface text-fg outline-none focus:border-accent transition"
                  />
                </div>
              );
            })}
          </div>
          <div className="px-4.5 py-3 border-t border-border flex justify-end gap-2">
            <button
              type="button"
              onClick={() => cancel(close)}
              className="px-4 h-8 rounded-md bg-surface-hi text-fg-2 text-[12px] font-medium hover:bg-border transition"
            >
              取消
            </button>
            <button
              type="button"
              disabled={!ok}
              onClick={() => submit(close)}
              className="px-4 h-8 rounded-md bg-accent text-white text-[12px] font-medium hover:opacity-90 disabled:opacity-50 disabled:cursor-not-allowed transition"
            >
              提交
            </button>
          </div>
        </>
      )}
    </Modal>
  );
}
