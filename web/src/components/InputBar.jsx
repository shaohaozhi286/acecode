// 输入框:textarea 自动撑高(最多 8 行) + Enter 发 / Shift+Enter 换行 +
// 上下键在首行/末行翻 history。
//
// 提交按钮在右侧悬浮(只在有内容时变蓝),空内容时灰色不可点。
//
// 斜杠命令:value 以 / 开头且无空白时,SlashDropdown 浮层显示在输入框上方。
// 选中后插入 `/<name> ` 到输入框,不立即发送(builtin 与 skill 行为统一)。
// 已识别的首段命令以 chip 样式叠加渲染(overlay div 与 textarea 同度量)。

import { forwardRef, useEffect, useImperativeHandle, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { getInputBarActionState } from '../lib/inputBarState.js';
import { VsIcon } from './Icon.jsx';
import { SlashDropdown } from './SlashDropdown.jsx';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { parseLeadingCommand } from '../lib/slashCommands.js';

const MAX_ROWS = 8;
const LINE_HEIGHT = 20; // 与 leading-[20px] 对齐

export const InputBar = forwardRef(function InputBar({
  disabled, placeholder = '输入消息或 / 命令…', onSubmit, onAbort, busy, history = [], variant = 'default',
}, ref) {
  const [value, setValue] = useState('');
  const [histPtr, setHistPtr] = useState(-1);
  const [dropdownClosed, setDropdownClosed] = useState(false); // Esc 关闭后,直到首段变化或重新输入 / 才重开
  const ta = useRef(null);
  const isHero = variant === 'hero';

  const slashCtx = useSlashCommands();
  const commands = slashCtx?.commands || [];
  const knownNames = useMemo(() => commands.map((c) => c.name), [commands]);

  useImperativeHandle(ref, () => ({
    focus: () => ta.current?.focus(),
    clear: () => setValue(''),
  }));

  const autosize = () => {
    const el = ta.current;
    if (!el) return;
    el.style.height = 'auto';
    const h = Math.min(el.scrollHeight, LINE_HEIGHT * MAX_ROWS + (isHero ? 28 : 16));
    el.style.height = h + 'px';
  };
  useEffect(autosize, [isHero, value]);

  // 触发条件:value 非空、首字符 /、整段无空白
  const showDropdownRaw = value.length > 0 && value[0] === '/' && !/\s/.test(value);
  const showDropdown = showDropdownRaw && !dropdownClosed && commands.length > 0;

  // value 变化:首段不再是 / 时复位 dropdownClosed,允许下次重新出现
  useEffect(() => {
    if (!showDropdownRaw) setDropdownClosed(false);
  }, [showDropdownRaw]);

  const handleSelectCommand = (item) => {
    if (!item) return;
    const next = '/' + item.name + ' ';
    setValue(next);
    setDropdownClosed(true);
    requestAnimationFrame(() => {
      const el = ta.current;
      if (el) {
        el.focus();
        el.setSelectionRange(next.length, next.length);
      }
    });
  };

  const submit = () => {
    const v = value.trim();
    if (!v || disabled) return;
    onSubmit?.(value);
    setValue('');
    setHistPtr(-1);
    setDropdownClosed(false);
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
    // 下拉打开时,Enter / Tab / Esc / 方向键 由 SlashDropdown 在捕获阶段处理。
    // 这里只处理常规情况。
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

  // 命令 chip overlay:首段是已知命令时,在 textarea 上叠一层透明 div,首段包 chip span。
  const leading = useMemo(() => parseLeadingCommand(value, knownNames), [value, knownNames]);
  const showChip = leading.name != null;
  const chipText = showChip ? value.slice(0, leading.headLength) : '';
  const restText = showChip ? value.slice(leading.headLength) : '';

  return (
    <div className={clsx(
      isHero ? 'ace-inputbar-hero' : 'border-t border-border px-2.5 py-2 bg-surface shrink-0',
    )}>
      <div className={clsx(
        'relative bg-surface border-[1.5px] border-border focus-within:border-accent focus-within:ring-2 focus-within:ring-accent/15 transition',
        isHero ? 'ace-inputbar-hero-card rounded-2xl' : 'rounded-xl',
      )}>
        {showDropdown && (
          <SlashDropdown
            items={commands}
            query={value.slice(1)}
            onSelect={handleSelectCommand}
            onClose={() => setDropdownClosed(true)}
          />
        )}
        {/* chip overlay: pointer-events:none, 透明文本,只让首段 chip 着色 */}
        {showChip && (
          <div
            aria-hidden="true"
            className={clsx(
              'absolute inset-0 pointer-events-none whitespace-pre-wrap break-words leading-[20px] font-sans overflow-hidden',
              isHero ? 'px-4 py-3 pr-14 text-[14px]' : 'px-3 py-2 pr-12 text-[13px]',
            )}
            style={{ color: 'transparent' }}
          >
            <span className="ace-slash-chip">{chipText}</span>
            <span>{restText}</span>
          </div>
        )}
        <textarea
          ref={ta}
          rows={1}
          value={value}
          onChange={(e) => setValue(e.target.value)}
          onKeyDown={onKey}
          disabled={disabled}
          placeholder={placeholder}
          className={clsx(
            'relative w-full resize-none bg-transparent border-0 outline-none leading-[20px] font-sans text-fg placeholder:text-fg-mute disabled:opacity-50',
            isHero ? 'px-4 py-3 pr-14 text-[14px]' : 'px-3 py-2 pr-12 text-[13px]',
          )}
          style={{ height: LINE_HEIGHT + (isHero ? 28 : 16) }}
        />
        <div className={clsx("absolute flex items-center gap-1", isHero ? "right-2.5 bottom-2.5" : "right-1.5 bottom-1")}>
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
      <div className={clsx('mt-1 px-1 text-[10px] text-fg-mute flex justify-between', isHero && 'px-3')}>
        <span>{actionState.helperText}</span>
      </div>
    </div>
  );
});
