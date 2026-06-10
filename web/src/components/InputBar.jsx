// 输入框:textarea 自动撑高(最多 8 行) + Enter 发 / Shift+Enter 换行 +
// 空输入或未编辑的历史项用上下键翻 history。
//
// 底部工具栏单独占一行,提交按钮在右侧(只在有内容时变蓝),空内容时灰色不可点。
//
// 斜杠命令:value 以 / 开头且无空白时,SlashDropdown 浮层显示在输入框上方。
// 选中后插入 `/<name> ` 到输入框,不立即发送(builtin 与 skill 行为统一)。
// 已识别的首段命令以原子 token 样式叠加渲染(overlay div 与 textarea 同度量)。

import { forwardRef, useCallback, useEffect, useImperativeHandle, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { getGoalStopControlState } from '../lib/goalControl.js';
import { getInputBarActionState } from '../lib/inputBarState.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';
import { SlashDropdown } from './SlashDropdown.jsx';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import {
  deleteLeadingCommandBlock,
  moveAcrossLeadingCommandBlock,
  normalizeLeadingCommandSelection,
  parseLeadingCommand,
  resolveLeadingSlashCommand,
  slashCommandKindPresentation,
} from '../lib/slashCommands.js';
import { getNextInputHistoryPointer, shouldNavigateInputHistory } from '../lib/inputHistoryNavigation.js';
import { filesFromClipboardEvent, filesFromTransfer, hasFileTransfer } from '../lib/composerFileTransfer.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import {
  SELECTION_CONTEXT_TYPE,
  contextPresentation,
} from '../lib/selectionChatContext.js';

const MAX_ROWS = 8;
const LINE_HEIGHT = 20; // 与 leading-[20px] 对齐

function composerAttachmentKey(item, index = 0) {
  return String(item?.local_id || item?.id || item?.name || index);
}

function composerAttachmentContext(item, index = 0) {
  const key = composerAttachmentKey(item, index);
  return {
    key,
    id: `composer:${key}`,
    name: item?.name || 'attachment',
    url: item?.preview_url || item?.blob_url || item?.url || '',
    path: item?.path || '',
  };
}

function composerContextKey(item, index = 0) {
  return String(item?.local_id || item?.id || item?.type || index);
}

function ComposerSelectionCard({ item, pinned = false, onPin, onRemove }) {
  const presentation = contextPresentation(item);
  const sourcePath = item?.source?.path || item?.path || presentation.label;
  const actionTitle = pinned ? '移除引用' : '固定引用';
  const actionLabel = pinned ? '移除引用上下文' : '固定引用上下文';
  return (
    <div
      className={[
        'group h-6 max-w-[260px] shrink-0 rounded-md border px-1.5 flex items-center gap-1 text-[11px] font-sans leading-none',
        pinned ? 'border-border bg-surface text-fg' : 'border-accent-soft bg-accent-bg text-fg',
      ].join(' ')}
      title={presentation.title}
    >
      <button
        type="button"
        className="w-[14px] h-[14px] shrink-0 rounded-full flex items-center justify-center hover:bg-surface-hi text-fg-mute hover:text-fg"
        onMouseDown={(event) => event.preventDefault()}
        onClick={() => {
          if (pinned) onRemove?.();
          else onPin?.();
        }}
        title={actionTitle}
        aria-label={actionLabel}
      >
        <VsIcon name={pinned ? 'close' : 'pin'} size={11} className="ace-selection-context-icon" />
      </button>
      <FileTypeIcon path={sourcePath} size={11} className="ace-selection-context-icon opacity-90" />
      <span className={['truncate text-fg', pinned ? '' : 'opacity-80'].filter(Boolean).join(' ')}>
        {presentation.label}
      </span>
    </div>
  );
}

export const InputBar = forwardRef(function InputBar({
  disabled, placeholder = '输入消息或 / 命令…', onSubmit, onAbort, busy, goal = null, goalStopping = false, history = [], variant = 'default',
  value: controlledValue, onChange,
  attachments = [], contexts = [], onMediaFiles, onRemoveAttachment, onAddBrowserContext, onRemoveContext,
  selectionPreview = null, onPinSelectionPreview,
}, ref) {
  const isControlled = controlledValue != null;
  const [internalValue, setInternalValue] = useState('');
  const value = isControlled ? String(controlledValue || '') : internalValue;
  const [histPtr, setHistPtr] = useState(-1);
  const [editedSinceHistory, setEditedSinceHistory] = useState(false);
  const [dropdownClosed, setDropdownClosed] = useState(false); // Esc 关闭后,直到首段变化或重新输入 / 才重开
  const [capabilityOpen, setCapabilityOpen] = useState(false);
  const [dragActive, setDragActive] = useState(false);
  const [attachmentPreview, setAttachmentPreview] = useState(null);
  const ta = useRef(null);
  const fileInputRef = useRef(null);
  const capabilityMenuRef = useRef(null);
  const dragDepthRef = useRef(0);
  const composingRef = useRef(false);
  const justFinishedCompositionRef = useRef(false);
  const compositionGuardTimerRef = useRef(0);
  const isHero = variant === 'hero';
  const textareaVerticalPadding = isHero ? 16 : 12;
  const textareaBaseHeight = LINE_HEIGHT + textareaVerticalPadding;
  const textareaMaxHeight = LINE_HEIGHT * MAX_ROWS + textareaVerticalPadding;
  const attachmentItems = Array.isArray(attachments) ? attachments : [];
  const contextItems = Array.isArray(contexts) ? contexts : [];
  const selectionContextItems = contextItems.filter((item) => item?.type === SELECTION_CONTEXT_TYPE);
  const otherContextItems = contextItems.filter((item) => item?.type !== SELECTION_CONTEXT_TYPE);
  const hasExtras = attachmentItems.length > 0 || contextItems.length > 0;
  const hasCapabilityHandlers = !!onMediaFiles || !!onAddBrowserContext;
  const isImageAttachment = (item) => String(item.kind || item.mime_type || '').startsWith('image');
  const imageAttachments = attachmentItems.filter(isImageAttachment);
  const fileAttachments = attachmentItems.filter((item) => !isImageAttachment(item));

  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      const { action, target } = detail;
      if (target?.type !== 'attachment' || !target.id || !target.id.startsWith('composer:')) return;
      const match = attachmentItems
        .map((item, index) => ({ item, context: composerAttachmentContext(item, index) }))
        .find(({ context }) => context.id === target.id);
      if (!match) return;
      if (action === DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT) {
        if (!match.context.url) return;
        detail.handled = true;
        setAttachmentPreview({ src: match.context.url, alt: match.context.name });
      } else if (action === DESKTOP_CONTEXT_ACTIONS.REMOVE_ATTACHMENT) {
        detail.handled = true;
        onRemoveAttachment?.(match.context.key);
      }
    };
    window.addEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
    return () => window.removeEventListener(DESKTOP_CONTEXT_ACTION_EVENT, handler);
  }, [attachmentItems, onRemoveAttachment]);

  const updateValue = useCallback((next) => {
    const text = String(next || '');
    if (!isControlled) setInternalValue(text);
    onChange?.(text);
  }, [isControlled, onChange]);

  const slashCtx = useSlashCommands();
  const commands = slashCtx?.commands || [];
  const knownNames = useMemo(() => commands.map((c) => c.name), [commands]);

  useImperativeHandle(ref, () => ({
    focus: () => ta.current?.focus(),
    clear: () => {
      updateValue('');
      setHistPtr(-1);
      setEditedSinceHistory(false);
    },
  }), [updateValue]);

  const autosize = () => {
    const el = ta.current;
    if (!el) return;
    el.style.height = 'auto';
    const h = Math.min(el.scrollHeight, textareaMaxHeight);
    el.style.height = h + 'px';
    el.style.overflowY = el.scrollHeight > textareaMaxHeight ? 'auto' : 'hidden';
  };
  useEffect(autosize, [textareaMaxHeight, value]);

  useEffect(() => () => {
    if (compositionGuardTimerRef.current) {
      window.clearTimeout(compositionGuardTimerRef.current);
    }
  }, []);

  const clearCompositionEndGuard = () => {
    if (compositionGuardTimerRef.current) {
      window.clearTimeout(compositionGuardTimerRef.current);
      compositionGuardTimerRef.current = 0;
    }
  };

  const handleCompositionStart = () => {
    clearCompositionEndGuard();
    composingRef.current = true;
    justFinishedCompositionRef.current = false;
  };

  const handleCompositionEnd = () => {
    composingRef.current = false;
    justFinishedCompositionRef.current = true;
    clearCompositionEndGuard();
    compositionGuardTimerRef.current = window.setTimeout(() => {
      justFinishedCompositionRef.current = false;
      compositionGuardTimerRef.current = 0;
    }, 0);
  };

  const isComposingKeyEvent = (event) => (
    composingRef.current ||
    justFinishedCompositionRef.current ||
    !!event.isComposing ||
    !!event.nativeEvent?.isComposing ||
    event.keyCode === 229 ||
    event.which === 229
  );

  // 触发条件:value 非空、首字符 /、整段无空白
  const showDropdownRaw = value.length > 0 && value[0] === '/' && !/\s/.test(value);
  const showDropdown = showDropdownRaw && !dropdownClosed && commands.length > 0;

  // value 变化:首段不再是 / 时复位 dropdownClosed,允许下次重新出现
  useEffect(() => {
    if (!showDropdownRaw) setDropdownClosed(false);
  }, [showDropdownRaw]);

  // 斜杠菜单从关闭→打开时重新拉一次 /api/commands:后端按 session cwd 当场扫盘,
  // 这样会话进行中(比如 agent 用 skill-creator)新写到磁盘的 skill 不必刷新页面
  // 就能出现在下拉里。只在进入命令态的那一刻触发,不是每次按键。
  useEffect(() => {
    if (showDropdownRaw) slashCtx?.invalidate?.();
  }, [showDropdownRaw]);

  const handleSelectCommand = (item) => {
    if (!item) return;
    const next = '/' + item.name + ' ';
    updateValue(next);
    setEditedSinceHistory(true);
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
    if ((!v && !hasExtras) || disabled) return;
    onSubmit?.(value);
    if (!isControlled) updateValue('');
    setHistPtr(-1);
    setEditedSinceHistory(false);
    setDropdownClosed(false);
    requestAnimationFrame(() => ta.current?.focus());
  };

  const focusTextareaSoon = useCallback(() => {
    requestAnimationFrame(() => ta.current?.focus());
  }, []);

  const addMediaFiles = useCallback((files) => {
    const fileList = Array.from(files || []).filter(Boolean);
    if (disabled || !onMediaFiles || fileList.length === 0) return false;
    setCapabilityOpen(false);
    onMediaFiles(fileList);
    focusTextareaSoon();
    return true;
  }, [disabled, focusTextareaSoon, onMediaFiles]);

  const chooseMedia = () => {
    setCapabilityOpen(false);
    fileInputRef.current?.click();
  };

  const handleFiles = (e) => {
    const files = Array.from(e.target.files || []);
    e.target.value = '';
    addMediaFiles(files);
  };

  const addBrowser = () => {
    setCapabilityOpen(false);
    onAddBrowserContext?.();
    focusTextareaSoon();
  };

  const resetDragState = useCallback(() => {
    dragDepthRef.current = 0;
    setDragActive(false);
  }, []);

  const handleDragEnter = useCallback((event) => {
    if (disabled || !onMediaFiles || !hasFileTransfer(event.dataTransfer)) return;
    event.preventDefault();
    dragDepthRef.current += 1;
    setDragActive(true);
  }, [disabled, onMediaFiles]);

  const handleDragOver = useCallback((event) => {
    if (disabled || !onMediaFiles || !hasFileTransfer(event.dataTransfer)) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
    setDragActive(true);
  }, [disabled, onMediaFiles]);

  const handleDragLeave = useCallback((event) => {
    if (!dragActive) return;
    dragDepthRef.current = Math.max(0, dragDepthRef.current - 1);
    if (dragDepthRef.current === 0) setDragActive(false);
  }, [dragActive]);

  const handleDrop = useCallback((event) => {
    const files = disabled || !onMediaFiles ? [] : filesFromTransfer(event.dataTransfer, { source: 'drop' });
    if (files.length > 0) {
      event.preventDefault();
      event.stopPropagation();
      addMediaFiles(files);
    }
    resetDragState();
  }, [addMediaFiles, disabled, onMediaFiles, resetDragState]);

  const handlePaste = useCallback((event) => {
    const files = disabled || !onMediaFiles ? [] : filesFromClipboardEvent(event);
    if (files.length === 0) return;
    event.preventDefault();
    event.stopPropagation();
    addMediaFiles(files);
  }, [addMediaFiles, disabled, onMediaFiles]);

  useEffect(() => {
    if (!dragActive) return undefined;
    window.addEventListener('dragend', resetDragState);
    window.addEventListener('drop', resetDragState);
    window.addEventListener('blur', resetDragState);
    return () => {
      window.removeEventListener('dragend', resetDragState);
      window.removeEventListener('drop', resetDragState);
      window.removeEventListener('blur', resetDragState);
    };
  }, [dragActive, resetDragState]);

  useEffect(() => {
    if (!hasCapabilityHandlers && capabilityOpen) setCapabilityOpen(false);
  }, [capabilityOpen, hasCapabilityHandlers]);

  useEffect(() => {
    if (!capabilityOpen) return undefined;

    const closeCapabilityMenu = () => setCapabilityOpen(false);
    const closeFromPointer = (event) => {
      const menu = capabilityMenuRef.current;
      if (menu && event.target instanceof Node && menu.contains(event.target)) return;
      closeCapabilityMenu();
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape') closeCapabilityMenu();
    };

    document.addEventListener('click', closeFromPointer, true);
    document.addEventListener('wheel', closeCapabilityMenu, true);
    document.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('blur', closeCapabilityMenu);
    window.addEventListener('resize', closeCapabilityMenu);
    return () => {
      document.removeEventListener('click', closeFromPointer, true);
      document.removeEventListener('wheel', closeCapabilityMenu, true);
      document.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('blur', closeCapabilityMenu);
      window.removeEventListener('resize', closeCapabilityMenu);
    };
  }, [capabilityOpen]);

  const handleChange = (e) => {
    const next = e.target.value;
    updateValue(next);
    setEditedSinceHistory(next.length > 0);
  };

  const leading = useMemo(() => parseLeadingCommand(value, knownNames), [value, knownNames]);
  const leadingCommand = useMemo(() => resolveLeadingSlashCommand(value, commands), [value, commands]);
  const showChip = leadingCommand != null;

  const setTextareaSelection = useCallback((selectionStart, selectionEnd) => {
    const el = ta.current;
    if (!el) return;
    el.setSelectionRange(selectionStart, selectionEnd);
  }, []);

  const normalizeCommandSelection = useCallback((target = ta.current) => {
    if (!showChip || !target) return;
    const edit = normalizeLeadingCommandSelection(
      value,
      leading,
      target.selectionStart,
      target.selectionEnd,
    );
    if (!edit) return;
    target.setSelectionRange(edit.selectionStart, edit.selectionEnd);
  }, [leading, showChip, value]);

  const normalizeCommandSelectionSoon = useCallback(() => {
    requestAnimationFrame(() => normalizeCommandSelection());
  }, [normalizeCommandSelection]);

  const onKey = (e) => {
    // 下拉打开时,Enter / Tab / Esc / 方向键 由 SlashDropdown 在捕获阶段处理。
    // 这里只处理常规情况。
    if (showChip) {
      const boundaryEdit = normalizeLeadingCommandSelection(
        value,
        leading,
        e.currentTarget.selectionStart,
        e.currentTarget.selectionEnd,
      );
      if (boundaryEdit) {
        e.currentTarget.setSelectionRange(boundaryEdit.selectionStart, boundaryEdit.selectionEnd);
      }
    }
    if ((e.key === 'ArrowLeft' || e.key === 'ArrowRight') && showChip && !e.shiftKey) {
      const move = moveAcrossLeadingCommandBlock(
        value,
        leading,
        e.currentTarget.selectionStart,
        e.currentTarget.selectionEnd,
        e.key === 'ArrowLeft' ? 'backward' : 'forward',
      );
      if (move) {
        e.preventDefault();
        setTextareaSelection(move.selectionStart, move.selectionEnd);
        return;
      }
    }
    if ((e.key === 'Backspace' || e.key === 'Delete') && showChip) {
      const edit = deleteLeadingCommandBlock(
        value,
        leading,
        e.currentTarget.selectionStart,
        e.currentTarget.selectionEnd,
        e.key === 'Delete' ? 'forward' : 'backward',
      );
      if (edit) {
        e.preventDefault();
        updateValue(edit.value);
        setEditedSinceHistory(edit.value.length > 0);
        setHistPtr(-1);
        setDropdownClosed(false);
        requestAnimationFrame(() => {
          const el = ta.current;
          if (el) {
            el.focus();
            el.setSelectionRange(edit.selectionStart, edit.selectionEnd);
          }
        });
        return;
      }
    }
    if (e.key === 'Enter' && !e.shiftKey) {
      if (isComposingKeyEvent(e)) return;
      e.preventDefault();
      submit();
      return;
    }
    if (shouldNavigateInputHistory({
      key: e.key,
      value,
      editedSinceHistory,
      historyLength: history.length,
      historyPointer: histPtr,
      altKey: e.altKey,
      ctrlKey: e.ctrlKey,
      metaKey: e.metaKey,
      shiftKey: e.shiftKey,
    })) {
      e.preventDefault();
      const next = getNextInputHistoryPointer({
        key: e.key,
        historyLength: history.length,
        historyPointer: histPtr,
      });
      if (next === -1) {
        setHistPtr(-1);
        updateValue('');
      } else {
        setHistPtr(next);
        updateValue(history[next] || '');
      }
      setEditedSinceHistory(false);
      return;
    }
  };

  const actionState = getInputBarActionState({ value, disabled, busy, hasExtras });
  const stopControl = getGoalStopControlState({ goal, busy, stopping: goalStopping });
  const textareaSpacingClass = isHero ? 'px-4 pt-3 pb-1 text-[14px]' : 'px-3 pt-2 pb-1 text-[13px]';

  // 命令 token overlay:首段是已知命令时,textarea 文字透明,由 overlay 负责着色渲染。
  // SVG 图标是额外视觉层,textarea 的真实 value 仍保持普通 "/command ..." 文本。
  const chipText = showChip ? leadingCommand.name : '';
  const restText = showChip ? leadingCommand.rest : '';
  const chipPresentation = showChip ? slashCommandKindPresentation(leadingCommand) : null;
  const slashChipStyleVars = showChip
    ? {
        '--ace-slash-base-left': isHero ? '16px' : '12px',
      }
    : null;

  return (
    <div className={clsx(
      isHero ? 'ace-inputbar-hero' : 'border-t border-border px-2.5 py-2 bg-surface shrink-0',
    )}>
      <input
        ref={fileInputRef}
        type="file"
        multiple
        className="hidden"
        onChange={handleFiles}
      />
      <div className={clsx(
        'relative bg-surface border-[1.5px] border-border focus-within:border-accent focus-within:ring-2 focus-within:ring-accent/15 transition',
        isHero ? 'ace-inputbar-hero-card rounded-2xl' : 'rounded-xl',
        dragActive && 'border-accent ring-2 ring-accent/20',
      )}
      onDragEnter={handleDragEnter}
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      onDrop={handleDrop}
      >
        {showDropdown && (
          <SlashDropdown
            items={commands}
            query={value.slice(1)}
            onSelect={handleSelectCommand}
            onClose={() => setDropdownClosed(true)}
          />
        )}
        {imageAttachments.length > 0 && (
          <div className={clsx(
            'px-3 pt-3 flex flex-wrap items-start gap-2',
            isHero && 'px-4',
          )}>
            {imageAttachments.map((item, index) => {
              const context = composerAttachmentContext(item, index);
              return (
                <div
                  key={context.key}
                  data-desktop-attachment-id={context.id}
                  data-desktop-attachment-name={context.name}
                  data-desktop-attachment-url={context.url || undefined}
                  data-desktop-attachment-path={context.path || undefined}
                  data-desktop-attachment-preview-url={context.url || undefined}
                  data-desktop-attachment-mutable="true"
                  className="group relative w-[86px] h-[86px] sm:w-24 sm:h-24 shrink-0 overflow-hidden rounded-lg border border-border bg-bg"
                >
                  {item.preview_url ? (
                    <img
                      src={item.preview_url}
                      alt=""
                      className="block w-full h-full object-cover bg-bg"
                    />
                  ) : (
                    <div className="w-full h-full bg-bg" />
                  )}
                  <button
                    type="button"
                    className="absolute right-[5px] top-[5px] w-[17px] h-[17px] rounded-full bg-black/75 hover:bg-black/85 text-white flex items-center justify-center"
                    onClick={() => onRemoveAttachment?.(context.key)}
                    aria-label="移除图片"
                  >
                    <VsIcon name="close" size={8} />
                  </button>
                </div>
              );
            })}
          </div>
        )}
        {(selectionPreview || selectionContextItems.length > 0) && (
          <div className={clsx(
            'px-3 pt-2 flex flex-wrap items-center gap-1.5',
            isHero && 'px-4',
          )}>
            {selectionPreview ? (
              <ComposerSelectionCard
                item={selectionPreview}
                onPin={() => onPinSelectionPreview?.(selectionPreview)}
              />
            ) : null}
            {selectionContextItems.map((item, index) => {
              const key = composerContextKey(item, index);
              return (
                <ComposerSelectionCard
                  key={key}
                  item={item}
                  pinned
                  onRemove={() => onRemoveContext?.(key)}
                />
              );
            })}
          </div>
        )}
        <div className="relative">
          {/* command overlay: pointer-events:none,与 textarea 使用相同度量。 */}
          {showChip && (
            <div
              aria-hidden="true"
              data-slash-chip-kind={leadingCommand.kind}
              data-slash-chip-icon={chipPresentation?.icon || ''}
              className={clsx(
                'absolute inset-0 pointer-events-none whitespace-pre-wrap break-words leading-[20px] font-sans text-fg overflow-hidden',
                textareaSpacingClass,
              )}
            >
              <span
                className="ace-slash-chip-unit"
                title={chipPresentation?.label || ''}
              >
                <span className="ace-slash-chip-icon">
                  <VsIcon name={chipPresentation?.icon || 'lightbulb'} size={14} />
                </span>
                <span className="ace-slash-chip">{chipText}</span>
              </span>
              <span>{restText}</span>
            </div>
          )}
          <textarea
            ref={ta}
            rows={1}
            value={value}
            onChange={handleChange}
            onKeyDown={onKey}
            onKeyUp={normalizeCommandSelectionSoon}
            onSelect={(event) => normalizeCommandSelection(event.currentTarget)}
            onClick={normalizeCommandSelectionSoon}
            onMouseUp={normalizeCommandSelectionSoon}
            onPaste={handlePaste}
            onCompositionStart={handleCompositionStart}
            onCompositionEnd={handleCompositionEnd}
            disabled={disabled}
            placeholder={placeholder}
            className={clsx(
              'relative w-full resize-none bg-transparent border-0 outline-none leading-[20px] font-sans placeholder:text-fg-mute disabled:opacity-50',
              showChip ? 'text-transparent' : 'text-fg',
              showChip && 'ace-slash-input-with-icon',
              textareaSpacingClass,
            )}
            style={{
              height: textareaBaseHeight,
              maxHeight: textareaMaxHeight,
              overflowY: 'hidden',
              caretColor: showChip ? 'var(--ace-fg)' : undefined,
              ...(slashChipStyleVars || {}),
            }}
          />
        </div>
        <div className={clsx("relative flex items-center gap-1 min-w-0 overflow-visible", isHero ? "px-2.5 pb-2.5" : "px-1.5 pb-1")}>
          <div className="flex min-w-0 flex-1 items-center gap-1 overflow-visible">
            <div ref={capabilityMenuRef} className="relative shrink-0 flex items-center">
              <button
                type="button"
                disabled={disabled || !hasCapabilityHandlers}
                className="w-7 h-7 rounded-full flex items-center justify-center text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50"
                onClick={() => setCapabilityOpen((open) => !open)}
                title="添加上下文"
              >
                <VsIcon name="add" size={15} />
              </button>
              {capabilityOpen && hasCapabilityHandlers && (
                <div className="absolute left-0 bottom-8 z-50 w-40 py-1 rounded-lg border border-border bg-surface ace-shadow">
                  <button
                    type="button"
                    className="w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] text-fg hover:bg-surface-hi disabled:opacity-50"
                    onClick={chooseMedia}
                    disabled={!onMediaFiles}
                  >
                    <VsIcon name="openFile" size={14} />
                    <span>添加图片或文件</span>
                  </button>
                  <button
                    type="button"
                    className="w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] text-fg hover:bg-surface-hi disabled:opacity-50"
                    onClick={addBrowser}
                    disabled={!onAddBrowserContext}
                  >
                    <VsIcon name="search" size={14} />
                    <span>浏览器</span>
                  </button>
                </div>
              )}
            </div>
            <div className="flex min-w-0 flex-1 items-center gap-1 overflow-hidden">
              {otherContextItems.map((item, index) => {
                const key = composerContextKey(item, index);
                const presentation = contextPresentation(item);
                return (
                  <div
                    key={key}
                    className="group h-7 max-w-[112px] shrink-0 rounded-md px-1.5 flex items-center gap-1 text-[12px] text-fg-mute hover:bg-surface-hi"
                    title={presentation.title}
                  >
                    <VsIcon name={presentation.icon} size={13} />
                    <span className="truncate">{presentation.label}</span>
                    <button
                      type="button"
                      className="w-4 h-4 rounded-full flex items-center justify-center hover:bg-bg text-fg-mute opacity-0 group-hover:opacity-100 focus:opacity-100"
                      onClick={() => onRemoveContext?.(key)}
                      aria-label={presentation.removeLabel}
                    >
                      <VsIcon name="close" size={9} />
                    </button>
                  </div>
                );
              })}
              {fileAttachments.map((item, index) => {
                const context = composerAttachmentContext(item, index);
                return (
                  <div
                    key={context.key}
                    data-desktop-attachment-id={context.id}
                    data-desktop-attachment-name={context.name}
                    data-desktop-attachment-url={context.url || undefined}
                    data-desktop-attachment-path={context.path || undefined}
                    data-desktop-attachment-preview-url={context.url || undefined}
                    data-desktop-attachment-mutable="true"
                    className="group h-7 max-w-[160px] min-w-0 rounded-md px-1.5 flex items-center gap-1 text-[12px] text-fg-mute hover:bg-surface-hi"
                    title={item.name}
                  >
                    <VsIcon name="file" size={13} />
                    <span className="truncate">{item.uploading ? `${item.name || '文件'} 上传中` : (item.name || '文件')}</span>
                    <button
                      type="button"
                      className="w-4 h-4 shrink-0 rounded-full flex items-center justify-center hover:bg-bg text-fg-mute opacity-0 group-hover:opacity-100 focus:opacity-100"
                      onClick={() => onRemoveAttachment?.(context.key)}
                      aria-label="移除文件"
                    >
                      <VsIcon name="close" size={9} />
                    </button>
                  </div>
                );
              })}
            </div>
          </div>
          <div className="flex shrink-0 items-center gap-1">
            {stopControl.visible && (
              <button
                type="button"
                onClick={onAbort}
                disabled={stopControl.disabled}
                className="px-2 h-7 rounded-md text-[11px] text-danger border border-danger/40 hover:bg-danger-bg transition flex items-center gap-1 disabled:opacity-50 disabled:cursor-wait"
                title={stopControl.title}
              >
                <VsIcon name="stop" size={12} mono={false} />
                <span>{stopControl.label}</span>
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
      </div>
      <div className={clsx('mt-1 px-1 text-[10px] text-fg-mute flex justify-between', isHero && 'px-3')}>
        <span>{actionState.helperText}</span>
      </div>
      <ImageLightbox preview={attachmentPreview} onClose={() => setAttachmentPreview(null)} />
    </div>
  );
});
