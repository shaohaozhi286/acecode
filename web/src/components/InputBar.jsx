// 输入框:富文本 composer 自动撑高(最多 8 行) + Enter 发 / Shift+Enter 换行 +
// 空输入或未编辑的历史项用上下键翻 history。
//
// 底部工具栏单独占一行,提交按钮在右侧(只在有内容时变蓝),空内容时灰色不可点。
//
// 斜杠命令:value 以 / 开头且无空白时,SlashDropdown 浮层显示在输入框上方。
// 选中后插入 `/<name> ` 到输入框,不立即发送(builtin 与 skill 行为统一)。
// 已识别的首段命令以原子 token 样式在同一 editable layout 内渲染。

import { forwardRef, useCallback, useEffect, useImperativeHandle, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { getGoalStopControlState } from '../lib/goalControl.js';
import { getInputBarActionState } from '../lib/inputBarState.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';
import { ComposerSessionControls } from './ComposerSessionControls.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';
import { RichComposer } from './RichComposer.jsx';
import { PathReferenceDropdown } from './PathReferenceDropdown.jsx';
import { SlashDropdown } from './SlashDropdown.jsx';
import { toast } from './Toast.jsx';
import { useSlashCommands } from './SlashCommandsContext.jsx';
import { getNextInputHistoryPointer, isUserComposerEdit, shouldNavigateInputHistory } from '../lib/inputHistoryNavigation.js';
import { filesFromTransfer, hasFileTransfer } from '../lib/composerFileTransfer.js';
import {
  captureComposerTextareaSelection,
  requestDesktopWindowFocus,
  restoreComposerTextareaCaret,
} from '../lib/composerCaretRestore.js';
import {
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
} from '../lib/desktopContextMenu.js';
import {
  SELECTION_CONTEXT_TYPE,
  contextPresentation,
} from '../lib/selectionChatContext.js';
import {
  insertPathReferenceAtCaret,
  normalizePathReferenceCandidates,
  pathReferenceSignature,
  pathReferenceTokenAtCursor,
  replacePathReferenceToken,
  splitPathReferenceQuery,
  unsafeReferencePath,
} from '../lib/pathReference.js';
import {
  hasNativeContextPicker,
  nativeFolderReferencePath,
  nativePickedFileToFile,
  parseNativeContextPickerResult,
} from '../lib/desktopContextPicker.js';

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
  pathReferenceApi = null, cwd = '',
  sessionControls = null,
}, ref) {
  const isControlled = controlledValue != null;
  const [internalValue, setInternalValue] = useState('');
  const value = isControlled ? String(controlledValue || '') : internalValue;
  const [histPtr, setHistPtr] = useState(-1);
  const [editedSinceHistory, setEditedSinceHistory] = useState(false);
  const [dropdownClosed, setDropdownClosed] = useState(false); // Esc 关闭后,直到首段变化或重新输入 / 才重开
  const [capabilityOpen, setCapabilityOpen] = useState(false);
  const [composerSelection, setComposerSelection] = useState({ start: 0, end: 0, direction: 'none' });
  const [composerComposing, setComposerComposing] = useState(false);
  const [pathMention, setPathMention] = useState(null);
  const [dragActive, setDragActive] = useState(false);
  const [attachmentPreview, setAttachmentPreview] = useState(null);
  const ta = useRef(null);
  const rootRef = useRef(null);
  const fileInputRef = useRef(null);
  const dismissedPathSignatureRef = useRef('');
  const mentionGenerationRef = useRef(0);
  const capabilityMenuRef = useRef(null);
  const dragDepthRef = useRef(0);
  const composingRef = useRef(false);
  const justFinishedCompositionRef = useRef(false);
  const compositionGuardTimerRef = useRef(0);
  const caretRestoreUntilRef = useRef(0);
  const caretRestoreSelectionRef = useRef(null);
  const caretRestoreScheduleRef = useRef({ firstRaf: 0, secondRaf: 0, timeout: 0 });
  const isHero = variant === 'hero';
  const textareaVerticalPadding = isHero ? 16 : 12;
  // 状态控制已经收进 composer，空输入区统一保留两行高度，整体比例与
  // WorkBuddy 式输入框一致；最大高度仍保持原有 8 行上限。
  const textareaBaseHeight = LINE_HEIGHT * 2 + textareaVerticalPadding;
  const textareaMaxHeight = LINE_HEIGHT * MAX_ROWS + textareaVerticalPadding;
  const attachmentItems = Array.isArray(attachments) ? attachments : [];
  const contextItems = Array.isArray(contexts) ? contexts : [];
  const selectionContextItems = contextItems.filter((item) => item?.type === SELECTION_CONTEXT_TYPE);
  const otherContextItems = contextItems.filter((item) => item?.type !== SELECTION_CONTEXT_TYPE);
  const hasExtras = attachmentItems.length > 0 || contextItems.length > 0;
  const nativeContextPickerAvailable = hasNativeContextPicker();
  const canChooseLocalContext = !!onMediaFiles || nativeContextPickerAvailable;
  const hasCapabilityHandlers = canChooseLocalContext || !!onAddBrowserContext;
  const isImageAttachment = (item) => String(item.kind || item.mime_type || '').startsWith('image');
  const imageAttachments = attachmentItems.filter(isImageAttachment);
  const fileAttachments = attachmentItems.filter((item) => !isImageAttachment(item));
  const composerLayoutSignature = useMemo(() => [
    ...attachmentItems.map((item, index) => [
      composerAttachmentKey(item, index),
      item?.id || '',
      item?.uploading ? 'uploading' : 'ready',
      item?.preview_url || '',
    ].join(':')),
    ...contextItems.map((item, index) => [
      composerContextKey(item, index),
      item?.type || '',
      item?.id || '',
    ].join(':')),
  ].join('\n'), [attachmentItems, contextItems]);

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

  useEffect(() => () => {
    if (compositionGuardTimerRef.current) {
      window.clearTimeout(compositionGuardTimerRef.current);
    }
  }, []);

  const clearCaretRestoreSchedule = useCallback(() => {
    const schedule = caretRestoreScheduleRef.current;
    if (schedule.firstRaf) window.cancelAnimationFrame(schedule.firstRaf);
    if (schedule.secondRaf) window.cancelAnimationFrame(schedule.secondRaf);
    if (schedule.timeout) window.clearTimeout(schedule.timeout);
    caretRestoreScheduleRef.current = { firstRaf: 0, secondRaf: 0, timeout: 0 };
  }, []);

  useEffect(() => () => {
    clearCaretRestoreSchedule();
  }, [clearCaretRestoreSchedule]);

  const restoreComposerCaretIfPending = useCallback(() => {
    const until = caretRestoreUntilRef.current;
    if (!until || Date.now() > until) return false;
    return restoreComposerTextareaCaret({
      textareaElement: ta.current,
      rootElement: rootRef.current,
      selection: caretRestoreSelectionRef.current,
      documentRef: typeof document === 'undefined' ? null : document,
    });
  }, []);

  const scheduleComposerCaretRestore = useCallback(() => {
    if (!caretRestoreUntilRef.current) return;
    clearCaretRestoreSchedule();
    caretRestoreScheduleRef.current.firstRaf = window.requestAnimationFrame(() => {
      caretRestoreScheduleRef.current.firstRaf = 0;
      restoreComposerCaretIfPending();
      caretRestoreScheduleRef.current.secondRaf = window.requestAnimationFrame(() => {
        caretRestoreScheduleRef.current.secondRaf = 0;
        restoreComposerCaretIfPending();
      });
      caretRestoreScheduleRef.current.timeout = window.setTimeout(() => {
        caretRestoreScheduleRef.current.timeout = 0;
        restoreComposerCaretIfPending();
      }, 80);
    });
  }, [clearCaretRestoreSchedule, restoreComposerCaretIfPending]);

  const requestComposerCaretRestore = useCallback(() => {
    caretRestoreUntilRef.current = Date.now() + 1500;
    caretRestoreSelectionRef.current = captureComposerTextareaSelection(ta.current);
    requestDesktopWindowFocus();
    restoreComposerTextareaCaret({
      textareaElement: ta.current,
      rootElement: rootRef.current,
      selection: caretRestoreSelectionRef.current,
      documentRef: typeof document === 'undefined' ? null : document,
      allowExternalFocus: true,
    });
    scheduleComposerCaretRestore();
  }, [scheduleComposerCaretRestore]);

  useLayoutEffect(() => {
    if (!caretRestoreUntilRef.current) return;
    scheduleComposerCaretRestore();
  }, [composerLayoutSignature, scheduleComposerCaretRestore]);

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
    setComposerComposing(true);
  };

  const handleCompositionEnd = () => {
    composingRef.current = false;
    setComposerComposing(false);
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
    mentionGenerationRef.current += 1;
    setPathMention(null);
    requestAnimationFrame(() => ta.current?.focus());
  };

  const focusTextareaSoon = useCallback(() => {
    requestAnimationFrame(() => ta.current?.focus());
  }, []);

  const restorePathCaret = useCallback((cursor) => {
    requestAnimationFrame(() => {
      const editor = ta.current;
      editor?.focus?.();
      editor?.setSelectionRange?.(cursor, cursor);
    });
  }, []);

  useImperativeHandle(ref, () => ({
    focus: () => ta.current?.focus(),
    clear: () => {
      updateValue('');
      setHistPtr(-1);
      setEditedSinceHistory(false);
    },
    insertDirectoryReference: (relativePath) => {
      const insertion = insertPathReferenceAtCaret(value, composerSelection.end, relativePath);
      updateValue(insertion.text);
      setEditedSinceHistory(true);
      restorePathCaret(insertion.cursor);
      return insertion;
    },
  }), [composerSelection.end, restorePathCaret, updateValue, value]);

  // Claude Code-style @ mention: only visible text is inserted. Directory
  // contents are never uploaded or preloaded here.
  useEffect(() => {
    const cursor = composerSelection.end;
    const token = pathReferenceTokenAtCursor(value, cursor);
    const signature = pathReferenceSignature(token, cursor, cwd);
    const unavailable = disabled || !pathReferenceApi || !cwd || composerComposing || showDropdown || !token;
    if (unavailable || dismissedPathSignatureRef.current === signature) {
      mentionGenerationRef.current += 1;
      setPathMention(null);
      return undefined;
    }
    dismissedPathSignatureRef.current = '';
    if (unsafeReferencePath(token.path)) {
      setPathMention({ token, signature, items: [], loading: false, error: '路径必须位于当前工作目录内' });
      return undefined;
    }
    const { directory, filter } = splitPathReferenceQuery(token.path);
    const generation = ++mentionGenerationRef.current;
    setPathMention({ token, signature, items: [], loading: true, error: '' });
    Promise.resolve(pathReferenceApi.listFiles(cwd, directory, true, true))
      .then((entries) => {
        if (mentionGenerationRef.current !== generation) return;
        setPathMention({
          token,
          signature,
          items: normalizePathReferenceCandidates(entries, filter),
          loading: false,
          error: '',
        });
      })
      .catch((error) => {
        if (mentionGenerationRef.current !== generation) return;
        setPathMention({
          token,
          signature,
          items: [],
          loading: false,
          error: error?.message || '目录读取失败',
        });
      });
    return () => { mentionGenerationRef.current += 1; };
  }, [composerComposing, composerSelection.end, cwd, disabled, pathReferenceApi, showDropdown, value]);

  useEffect(() => {
    if (!disabled && pathReferenceApi && cwd) return;
    mentionGenerationRef.current += 1;
    setPathMention(null);
  }, [cwd, disabled, pathReferenceApi]);

  const closePathDropdown = useCallback(() => {
    if (pathMention?.signature) dismissedPathSignatureRef.current = pathMention.signature;
    mentionGenerationRef.current += 1;
    setPathMention(null);
  }, [pathMention?.signature]);

  const applyMentionItem = useCallback((item, enterDirectory = false) => {
    if (!pathMention?.token || !item) return;
    const replacement = replacePathReferenceToken(value, pathMention.token, item.path, {
      directory: item.kind === 'dir',
      enterDirectory,
    });
    mentionGenerationRef.current += 1;
    setPathMention(null);
    updateValue(replacement.text);
    setEditedSinceHistory(true);
    restorePathCaret(replacement.cursor);
  }, [pathMention?.token, restorePathCaret, updateValue, value]);

  const activePathDropdown = pathMention;

  const addMediaFiles = useCallback((files) => {
    const fileList = Array.from(files || []).filter(Boolean);
    if (disabled || !onMediaFiles || fileList.length === 0) return false;
    setCapabilityOpen(false);
    requestComposerCaretRestore();
    onMediaFiles(fileList);
    return true;
  }, [disabled, onMediaFiles, requestComposerCaretRestore]);

  const chooseLocalContext = useCallback(async () => {
    setCapabilityOpen(false);
    if (!nativeContextPickerAvailable) {
      fileInputRef.current?.click();
      return;
    }

    const savedCursor = composerSelection.end;
    try {
      const raw = await window.aceDesktop_pickContextItems({ cwd });
      const picked = parseNativeContextPickerResult(raw);
      if (picked.cancelled) {
        restorePathCaret(savedCursor);
        return;
      }
      if (picked.folder) {
        const referencePath = nativeFolderReferencePath(cwd, picked.folder);
        const insertion = insertPathReferenceAtCaret(value, savedCursor, referencePath);
        updateValue(insertion.text);
        setEditedSinceHistory(true);
        restorePathCaret(insertion.cursor);
        return;
      }

      const files = picked.files.map((item) => nativePickedFileToFile(item));
      if (!addMediaFiles(files)) restorePathCaret(savedCursor);
    } catch (error) {
      toast({ kind: 'err', text: `添加文件或文件夹失败:${error?.message || '选择器不可用'}` });
      restorePathCaret(savedCursor);
    }
  }, [
    addMediaFiles,
    composerSelection.end,
    cwd,
    nativeContextPickerAvailable,
    restorePathCaret,
    updateValue,
    value,
  ]);

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

  const handleComposerChange = (next) => {
    // Lexical 的 onChange 回声(程序化设值同步 / 光标移动)文本与当前 value 相同,
    // 不算用户编辑 —— 否则历史导航刚填入的文本会被误标为已编辑,上下键随即失效。
    if (!isUserComposerEdit({ nextValue: next, currentValue: value })) return;
    updateValue(next);
    setEditedSinceHistory(next.length > 0);
  };

  const onKey = (e) => {
    // 下拉打开时,Enter / Tab / Esc / 方向键 由 SlashDropdown 在捕获阶段处理。
    // 这里只处理常规情况。
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
  const composerSpacingClass = isHero ? 'px-4 pt-3 pb-1 text-[14px]' : 'px-3 pt-2 pb-1 text-[13px]';
  const hasInlineContexts = otherContextItems.length > 0 || fileAttachments.length > 0;
  const capabilityControl = (
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
        <div className="absolute left-0 bottom-8 z-50 w-52 py-1 rounded-lg border border-border bg-surface ace-shadow">
          <button
            type="button"
            className="w-full h-8 px-2 flex items-center gap-2 text-left text-[13px] text-fg hover:bg-surface-hi disabled:opacity-50"
            onClick={chooseLocalContext}
            disabled={!canChooseLocalContext}
          >
            <VsIcon name="openFile" size={14} />
            <span>{nativeContextPickerAvailable ? '添加图片、文件或文件夹' : '添加图片或文件'}</span>
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
  );
  const inlineContextControls = hasInlineContexts ? (
    <>
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
            className="group h-7 max-w-[160px] min-w-0 shrink-0 rounded-md px-1.5 flex items-center gap-1 text-[12px] text-fg-mute hover:bg-surface-hi"
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
    </>
  ) : null;
  const submitControls = (
    <>
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
    </>
  );

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
        'ace-composer-card relative bg-surface border-[1.5px] border-border focus-within:border-accent focus-within:ring-2 focus-within:ring-accent/15 transition',
        isHero ? 'ace-inputbar-hero-card rounded-2xl' : 'rounded-xl',
        dragActive && 'border-accent ring-2 ring-accent/20',
      )}
      ref={rootRef}
      onDragEnter={handleDragEnter}
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      onDrop={handleDrop}
      >
        {activePathDropdown && (
          <PathReferenceDropdown
            items={activePathDropdown.items || []}
            loading={!!activePathDropdown.loading}
            error={activePathDropdown.error || ''}
            onReference={(item) => applyMentionItem(item, false)}
            onEnterDirectory={(item) => applyMentionItem(item, true)}
            onClose={closePathDropdown}
          />
        )}
        {showDropdown && !activePathDropdown && (
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
                  className={clsx(
                    'group relative w-[86px] h-[86px] sm:w-24 sm:h-24 shrink-0 overflow-hidden rounded-lg border border-border bg-bg',
                    context.url ? 'cursor-zoom-in hover:border-accent-soft' : '',
                  )}
                  onClick={context.url ? () => setAttachmentPreview({ src: context.url, alt: context.name }) : undefined}
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
                    onClick={(e) => { e.stopPropagation(); onRemoveAttachment?.(context.key); }}
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
          <RichComposer
            ref={ta}
            value={value}
            commands={commands}
            onChange={handleComposerChange}
            onKeyDown={onKey}
            onCompositionStart={handleCompositionStart}
            onCompositionEnd={handleCompositionEnd}
            onSelectionChange={setComposerSelection}
            isComposingKeyEvent={isComposingKeyEvent}
            onSubmit={submit}
            onPasteFiles={addMediaFiles}
            disabled={disabled}
            placeholder={placeholder}
            className={clsx(
              'ace-rich-composer-input relative w-full bg-transparent border-0 outline-none leading-[20px] font-sans text-fg disabled:opacity-50 whitespace-pre-wrap break-words',
              composerSpacingClass,
            )}
            placeholderClassName={composerSpacingClass}
            style={{
              minHeight: textareaBaseHeight,
              maxHeight: textareaMaxHeight,
              overflowY: 'auto',
            }}
          />
        </div>
        <ComposerSessionControls
          {...(sessionControls || {})}
          className={isHero ? 'px-2.5 pb-2.5' : 'px-1.5 pb-1'}
          addControl={capabilityControl}
          contexts={inlineContextControls}
          actions={submitControls}
        />
      </div>
      <ImageLightbox preview={attachmentPreview} onClose={() => setAttachmentPreview(null)} />
    </div>
  );
});
