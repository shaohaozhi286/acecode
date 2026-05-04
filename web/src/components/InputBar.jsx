// 输入框:textarea 自动撑高(最多 8 行) + Enter 发 / Shift+Enter 换行 +
// 上下键在首行/末行翻 history。
//
// 提交按钮在右侧悬浮(只在有内容时变蓝),空内容时灰色不可点。

import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { getInputBarActionState } from '../lib/inputBarState.js';
import { VsIcon } from './Icon.jsx';

const MAX_ROWS = 8;
const LINE_HEIGHT = 20; // 与 leading-[20px] 对齐

export const InputBar = forwardRef(function InputBar({
  disabled, placeholder = '输入消息或 / 命令…', onSubmit, onAbort, busy, history = [],
}, ref) {
  const [value, setValue] = useState('');
  const [histPtr, setHistPtr] = useState(-1);
  const ta = useRef(null);

  useImperativeHandle(ref, () => ({
    focus: () => ta.current?.focus(),
    clear: () => setValue(''),
  }));

  const autosize = () => {
    const el = ta.current;
    if (!el) return;
    el.style.height = 'auto';
    const h = Math.min(el.scrollHeight, LINE_HEIGHT * MAX_ROWS + 16);
    el.style.height = h + 'px';
  };
  useEffect(autosize, [value]);

  const submit = () => {
    const v = value.trim();
    if (!v || disabled) return;
    onSubmit?.(value);
    setValue('');
    setHistPtr(-1);
    requestAnimationFrame(() => ta.current?.focus());
  };

  const atFirstLine = () => {
    const el = ta.current; if (!el) return true;
    return !el.value.substring(0, el.selectionStart).includes('\n');
  };
  const atLastLine = () => {
    const el = ta.current; if (!el) return true;
    return !el.value.substring(el.selectionEnd).includes('\n');
  };

  const onKey = (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      submit();
      return;
    }
    if (e.key === 'ArrowUp' && atFirstLine() && history.length) {
      e.preventDefault();
      const next = histPtr === -1 ? history.length - 1 : Math.max(0, histPtr - 1);
      setHistPtr(next);
      setValue(history[next] || '');
      return;
    }
    if (e.key === 'ArrowDown' && atLastLine() && histPtr !== -1) {
      e.preventDefault();
      const next = histPtr + 1;
      if (next >= history.length) { setHistPtr(-1); setValue(''); }
      else                         { setHistPtr(next); setValue(history[next]); }
    }
  };

  const actionState = getInputBarActionState({ value, disabled, busy });
  const hasText = actionState.hasText;

  return (
    <div className="border-t border-border px-2.5 py-2 bg-surface shrink-0">
      <div className="relative bg-surface border-[1.5px] border-border rounded-xl focus-within:border-accent focus-within:ring-2 focus-within:ring-accent/15 transition">
        <textarea
          ref={ta}
          rows={1}
          value={value}
          onChange={(e) => setValue(e.target.value)}
          onKeyDown={onKey}
          disabled={disabled}
          placeholder={placeholder}
          className="w-full resize-none bg-transparent border-0 outline-none px-3 py-2 pr-12 text-[13px] leading-[20px] font-sans text-fg placeholder:text-fg-mute disabled:opacity-50"
          style={{ height: LINE_HEIGHT + 16 }}
        />
        <div className="absolute right-1.5 bottom-1.5 flex items-center gap-1">
          {busy && (
            <button
              type="button"
              onClick={onAbort}
              className="px-2 h-7 rounded-md text-[11px] text-danger border border-danger/40 hover:bg-danger-bg transition flex items-center gap-1"
              title="中断当前任务"
            >
              <VsIcon name="stop" size={12} mono={false} />
              <span>中断</span>
            </button>
          )}
          {busy ? (
            <button
              type="button"
              onClick={submit}
              disabled={!actionState.canSubmit}
              className={clsx(
                'px-2 h-7 rounded-md text-[11px] transition flex items-center gap-1',
                actionState.canSubmit
                  ? 'bg-accent text-white hover:opacity-90'
                  : 'bg-surface-hi text-fg-mute cursor-default',
              )}
              title={actionState.submitTitle}
            >
              <VsIcon name="send" size={12} mono={false} className={actionState.canSubmit ? 'ace-icon-on-accent' : ''} />
              <span>{actionState.submitLabel}</span>
            </button>
          ) : (
            <button
              type="button"
              onClick={submit}
              disabled={!actionState.canSubmit}
              className={clsx(
                'w-7 h-7 rounded-full flex items-center justify-center transition',
                actionState.canSubmit
                  ? 'bg-accent text-white hover:opacity-90'
                  : 'bg-surface-hi text-fg-mute cursor-default',
              )}
              title={actionState.submitTitle}
            >
              <VsIcon name="send" size={14} mono={false} className={actionState.canSubmit ? 'ace-icon-on-accent' : ''} />
            </button>
          )}
        </div>
      </div>
      <div className="mt-1 px-1 text-[10px] text-fg-mute flex justify-between">
        <span>{actionState.helperText}</span>
        {value.startsWith('/') && (
          <span className="text-accent">/ 命令模式</span>
        )}
      </div>
    </div>
  );
});
