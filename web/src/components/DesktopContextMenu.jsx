import { useEffect, useMemo, useRef, useState } from 'react';
import {
  buildDesktopContextMenuItems,
  clampContextMenuPosition,
  DESKTOP_CONTEXT_ACTIONS,
  SESSION_PIN_TOGGLE_EVENT,
  openInExplorerTargetFromElement,
  sessionPinTargetFromElement,
} from '../lib/desktopContextMenu.js';
import { toast } from './Toast.jsx';

const MENU_WIDTH = 176;
const MENU_ROW_HEIGHT = 30;
const MENU_PADDING = 8;

const ACTION_LABELS = {
  [DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER]: '在资源管理器中打开',
  [DESKTOP_CONTEXT_ACTIONS.PIN_SESSION]: '置顶',
  [DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION]: '取消置顶',
  [DESKTOP_CONTEXT_ACTIONS.SELECT_ALL]: '全选',
  [DESKTOP_CONTEXT_ACTIONS.COPY]: '复制',
  [DESKTOP_CONTEXT_ACTIONS.PASTE]: '粘贴',
  [DESKTOP_CONTEXT_ACTIONS.CUT]: '剪切',
  [DESKTOP_CONTEXT_ACTIONS.INSPECT]: '检查',
};

const TEXT_INPUT_TYPES = new Set([
  '',
  'email',
  'password',
  'search',
  'tel',
  'text',
  'url',
]);

function isDesktopShell() {
  return !!(window.__ACECODE_DESKTOP_SHELL__ || window.aceDesktop_openDevTools || window.aceDesktop_openInExplorer);
}

function parseDesktopResult(value) {
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

function editableElementFrom(node) {
  let el = node instanceof Element ? node : null;
  while (el) {
    if (el instanceof HTMLTextAreaElement) {
      return el.disabled || el.readOnly ? null : el;
    }
    if (el instanceof HTMLInputElement) {
      const type = String(el.type || '').toLowerCase();
      return !el.disabled && !el.readOnly && TEXT_INPUT_TYPES.has(type) ? el : null;
    }
    if (el.isContentEditable) return el;
    el = el.parentElement;
  }
  return null;
}

function selectedTextForTarget(target) {
  const editable = editableElementFrom(target);
  if (editable instanceof HTMLInputElement || editable instanceof HTMLTextAreaElement) {
    const start = editable.selectionStart ?? 0;
    const end = editable.selectionEnd ?? start;
    return start === end ? '' : editable.value.slice(start, end);
  }
  return window.getSelection?.().toString() || '';
}

function selectEditableContents(editable) {
  editable.focus();
  if (editable instanceof HTMLInputElement || editable instanceof HTMLTextAreaElement) {
    editable.select();
    return;
  }
  const range = document.createRange();
  range.selectNodeContents(editable);
  const sel = window.getSelection();
  sel?.removeAllRanges();
  sel?.addRange(range);
}

function selectAllForTarget(target) {
  const editable = editableElementFrom(target);
  if (editable) {
    selectEditableContents(editable);
    return;
  }
  const sel = window.getSelection();
  if (!sel || !document.body) return;
  sel.removeAllRanges();
  const range = document.createRange();
  range.selectNodeContents(document.body);
  sel.addRange(range);
}

function insertTextIntoEditable(editable, text) {
  editable.focus();
  if (editable instanceof HTMLInputElement || editable instanceof HTMLTextAreaElement) {
    const start = editable.selectionStart ?? editable.value.length;
    const end = editable.selectionEnd ?? start;
    editable.setRangeText(text, start, end, 'end');
    editable.dispatchEvent(new InputEvent('input', {
      bubbles: true,
      cancelable: true,
      data: text,
      inputType: 'insertFromPaste',
    }));
    return;
  }
  document.execCommand('insertText', false, text);
}

async function copySelectionFromTarget(target, rememberedText = '') {
  const text = selectedTextForTarget(target) || rememberedText;
  if (text && navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(text);
    return;
  }
  document.execCommand('copy');
}

async function pasteIntoTarget(target) {
  const editable = editableElementFrom(target);
  if (!editable) return;
  if (navigator.clipboard?.readText) {
    const text = await navigator.clipboard.readText();
    insertTextIntoEditable(editable, text);
    return;
  }
  editable.focus();
  document.execCommand('paste');
}

async function openTargetInExplorer(openTarget) {
  if (!openTarget?.path || typeof window.aceDesktop_openInExplorer !== 'function') {
    toast({ kind: 'err', text: '无法打开:desktop bridge 不可用' });
    return;
  }
  try {
    const result = parseDesktopResult(await window.aceDesktop_openInExplorer(openTarget.path));
    if (!result?.ok) {
      toast({ kind: 'err', text: '打开失败:' + (result?.error || '') });
      return;
    }
    toast({ kind: 'ok', text: '已在资源管理器中打开' });
  } catch (e) {
    toast({ kind: 'err', text: '打开异常:' + (e?.message || '') });
  }
}

function dispatchSessionPinToggle(sessionPinTarget, nextPinned) {
  if (!sessionPinTarget?.sessionId) return;
  window.dispatchEvent(new CustomEvent(SESSION_PIN_TOGGLE_EVENT, {
    detail: {
      ...sessionPinTarget,
      pinned: !!nextPinned,
    },
  }));
}

async function runAction(action, target, rememberedText = '', openTarget = null, sessionPinTarget = null) {
  switch (action) {
    case DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER:
      await openTargetInExplorer(openTarget);
      break;
    case DESKTOP_CONTEXT_ACTIONS.PIN_SESSION:
      dispatchSessionPinToggle(sessionPinTarget, true);
      break;
    case DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION:
      dispatchSessionPinToggle(sessionPinTarget, false);
      break;
    case DESKTOP_CONTEXT_ACTIONS.SELECT_ALL:
      selectAllForTarget(target);
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY:
      await copySelectionFromTarget(target, rememberedText);
      break;
    case DESKTOP_CONTEXT_ACTIONS.PASTE:
      await pasteIntoTarget(target);
      break;
    case DESKTOP_CONTEXT_ACTIONS.CUT:
      editableElementFrom(target)?.focus();
      document.execCommand('cut');
      break;
    case DESKTOP_CONTEXT_ACTIONS.INSPECT:
      await window.aceDesktop_openDevTools?.();
      break;
    default:
      break;
  }
}

export function DesktopContextMenu() {
  const [menu, setMenu] = useState(null);
  const targetRef = useRef(null);
  const lastSelectionRef = useRef({ target: null, text: '' });
  const desktop = useMemo(() => isDesktopShell(), []);

  useEffect(() => {
    if (!desktop) return undefined;

    const rememberSelection = () => {
      const target = document.activeElement;
      const text = selectedTextForTarget(target);
      if (text) lastSelectionRef.current = { target: editableElementFrom(target) || target, text };
    };
    const close = () => setMenu(null);
    const closeFromPointer = (event) => {
      if (event.target instanceof Element && event.target.closest('.ace-desktop-context-menu')) return;
      close();
    };
    const onContextMenu = (event) => {
      event.preventDefault();
      event.stopPropagation();

      const target = event.target;
      targetRef.current = target;
      const editableTarget = editableElementFrom(target);
      const editable = !!editableTarget;
      const openTarget = editable ? null : openInExplorerTargetFromElement(target);
      const sessionPinTarget = editable ? null : sessionPinTargetFromElement(target);
      let selectedText = selectedTextForTarget(target);
      if (!selectedText && editableTarget && lastSelectionRef.current.target === editableTarget) {
        selectedText = lastSelectionRef.current.text;
      }
      const hasSelection = selectedText.length > 0;
      const debug = !!window.__ACECODE_DESKTOP_DEBUG__;
      const items = buildDesktopContextMenuItems({
        editable,
        hasSelection,
        debug,
        openInExplorer: !!openTarget,
        sessionPinTarget,
      });
      const pos = clampContextMenuPosition({
        x: event.clientX,
        y: event.clientY,
        width: MENU_WIDTH,
        height: items.length * MENU_ROW_HEIGHT + MENU_PADDING,
        viewportWidth: window.innerWidth,
        viewportHeight: window.innerHeight,
      });
      setMenu({ ...pos, items, selectedText, openTarget, sessionPinTarget });
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape') close();
    };

    document.addEventListener('contextmenu', onContextMenu, true);
    document.addEventListener('selectionchange', rememberSelection);
    document.addEventListener('select', rememberSelection, true);
    document.addEventListener('click', closeFromPointer, true);
    document.addEventListener('wheel', close, true);
    document.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('blur', close);
    window.addEventListener('resize', close);
    return () => {
      document.removeEventListener('contextmenu', onContextMenu, true);
      document.removeEventListener('selectionchange', rememberSelection);
      document.removeEventListener('select', rememberSelection, true);
      document.removeEventListener('click', closeFromPointer, true);
      document.removeEventListener('wheel', close, true);
      document.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('blur', close);
      window.removeEventListener('resize', close);
    };
  }, [desktop]);

  if (!desktop || !menu) return null;

  return (
    <div
      className="ace-desktop-context-menu"
      style={{ left: menu.left, top: menu.top }}
      role="menu"
      onMouseDown={(event) => event.stopPropagation()}
      onClick={(event) => event.stopPropagation()}
    >
      {menu.items.map((action) => (
        <button
          key={action}
          type="button"
          role="menuitem"
          className="ace-desktop-context-menu-item"
          onClick={async () => {
            const target = targetRef.current;
            const selectedText = menu.selectedText || '';
            const openTarget = menu.openTarget || null;
            const sessionPinTarget = menu.sessionPinTarget || null;
            setMenu(null);
            await runAction(action, target, selectedText, openTarget, sessionPinTarget);
          }}
        >
          {ACTION_LABELS[action] || action}
        </button>
      ))}
    </div>
  );
}
