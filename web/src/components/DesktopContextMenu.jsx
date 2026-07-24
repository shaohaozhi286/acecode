import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  buildDesktopContextMenuItems,
  clampContextMenuPosition,
  contextTargetsFromElement,
  contextMenuOpenDelay,
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  SESSION_PIN_TOGGLE_EVENT,
  shouldUseCustomContextMenu,
} from '../lib/desktopContextMenu.js';
import { exportMermaidAsset } from '../lib/mermaidExport.js';
import { selectionContextFromWindowSelection } from '../lib/selectionChatContext.js';
import { copyImageToSystemClipboard, copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { isDesktopShell, isWebappCompat } from '../lib/desktopShellMode.js';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';

const MENU_WIDTH = 216;
const MENU_ROW_HEIGHT = 30;
const MENU_PADDING = 8;

const ACTION_LABELS = {
  [DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER]: '在资源管理器中打开',
  [DESKTOP_CONTEXT_ACTIONS.LOCATE_FILE]: '在资源管理器中显示',
  [DESKTOP_CONTEXT_ACTIONS.PIN_SESSION]: '置顶',
  [DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION]: '取消置顶',
  [DESKTOP_CONTEXT_ACTIONS.OPEN_SESSION]: '打开会话',
  [DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION]: '重命名会话',
  [DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE]: '复制会话标题',
  [DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_ID]: '复制会话 ID',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_SESSION]: '导出会话',
  [DESKTOP_CONTEXT_ACTIONS.ARCHIVE_SESSION]: '归档会话',
  [DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE]: '切换到项目',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_WORKSPACE]: '展开项目',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_WORKSPACE]: '折叠项目',
  [DESKTOP_CONTEXT_ACTIONS.NEW_WORKSPACE_SESSION]: '新建会话',
  [DESKTOP_CONTEXT_ACTIONS.IMPORT_OPENCODE_SESSIONS]: '从opencode导入会话',
  [DESKTOP_CONTEXT_ACTIONS.RENAME_WORKSPACE]: '重命名项目',
  [DESKTOP_CONTEXT_ACTIONS.COPY_WORKSPACE_PATH]: '复制项目路径',
  [DESKTOP_CONTEXT_ACTIONS.REMOVE_WORKSPACE]: '从项目列表移除',
  [DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE]: '预览文件',
  [DESKTOP_CONTEXT_ACTIONS.REFRESH_DETAILS]: '刷新',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TAB]: '关闭',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_OTHER_PREVIEW_TABS]: '关闭其他',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TABS_TO_RIGHT]: '关闭右侧标签页',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_ALL_PREVIEW_TABS]: '全部关闭',
  [DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH]: '复制相对路径',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH]: '复制绝对路径',
  [DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT]: '加入输入上下文',
  [DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT]: '加入输入上下文',
  [DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT]: '引用到聊天',
  [DESKTOP_CONTEXT_ACTIONS.REFRESH_FILE_TREE]: '刷新文件树',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_DIRECTORY]: '展开目录',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_DIRECTORY]: '折叠目录',
  [DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_TEXT]: '复制预览内容',
  [DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_METADATA]: '复制预览信息',
  [DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_IMAGE]: '复制图片',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_PNG]: '导出 PNG 图片',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SVG]: '导出 SVG',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SOURCE]: '导出 Mermaid 源码',
  [DESKTOP_CONTEXT_ACTIONS.COPY_FILE_DIFF]: '复制此文件 diff',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ALL_DIFFS]: '复制全部 diff',
  [DESKTOP_CONTEXT_ACTIONS.LOCATE_IN_FILE_TREE]: '在文件树中定位',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_ALL_DIFFS]: '展开全部 diff',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_ALL_DIFFS]: '折叠全部 diff',
  [DESKTOP_CONTEXT_ACTIONS.COPY_MESSAGE_TEXT]: '复制消息',
  [DESKTOP_CONTEXT_ACTIONS.FORK_MESSAGE]: '从这里分叉',
  [DESKTOP_CONTEXT_ACTIONS.COPY_VISIBLE_TOOL_OUTPUT]: '复制可见输出',
  [DESKTOP_CONTEXT_ACTIONS.COPY_FULL_TOOL_OUTPUT]: '复制完整输出',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_TOOL]: '展开工具详情',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_TOOL]: '收起工具详情',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE]: '复制图片',
  [DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT]: '预览附件',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME]: '复制附件名',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL]: '复制附件地址',
  [DESKTOP_CONTEXT_ACTIONS.REMOVE_ATTACHMENT]: '移除附件',
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
  if (text) {
    const result = await copyTextToSystemClipboard(text);
    if (!result?.ok) throw new Error(result?.error || 'clipboard unavailable');
    return;
  }
  document.execCommand('copy');
}

async function copyTextWithToast(text, label = '已复制') {
  const result = await copyTextToSystemClipboard(text);
  if (result?.ok) {
    toast({ kind: 'ok', text: label });
  } else {
    toast({ kind: 'err', text: '复制失败:' + (result?.error || '') });
  }
}

async function exportMermaidWithToast(target, format, label) {
  const result = await exportMermaidAsset(target, format);
  if (result?.ok) {
    toast({ kind: 'ok', text: `已导出 ${label}` });
  } else {
    toast({ kind: 'err', text: '导出失败:' + (result?.error || '') });
  }
  return result;
}

export async function copyImageWithToast(target) {
  const result = await copyImageToSystemClipboard(target?.copyImageUrl || target?.previewUrl || '', {
    mimeType: target?.mimeType || target?.contentType || '',
  });
  if (result?.ok) {
    toast({ kind: 'ok', text: '已复制图片' });
  } else {
    toast({ kind: 'err', text: '复制图片失败:' + (result?.error || '') });
  }
  return result;
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
  if (!openTarget?.path) {
    toast({ kind: 'err', text: '无法打开:路径为空' });
    return;
  }
  try {
    let result;
    if (typeof window.aceDesktop_openInExplorer === 'function') {
      result = parseDesktopResult(await window.aceDesktop_openInExplorer(openTarget.path));
    } else {
      // webapp 兼容模式没有 webview bridge,daemon 就在本机,改走 REST 端点。
      result = await api.openInExplorer(openTarget.path);
    }
    if (!result?.ok) {
      toast({ kind: 'err', text: '打开失败:' + (result?.error || '') });
      return;
    }
    toast({ kind: 'ok', text: '已在资源管理器中打开' });
  } catch (e) {
    const detail = e?.body?.error || e?.message || '';
    toast({ kind: 'err', text: '打开失败:' + detail });
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

function dispatchDesktopContextAction(action, targetPayload, extra = {}) {
  const detail = {
    action,
    target: targetPayload || null,
    handled: false,
    ...extra,
  };
  window.dispatchEvent(new CustomEvent(DESKTOP_CONTEXT_ACTION_EVENT, { detail }));
  return !!detail.handled;
}

async function runAction(item, target, rememberedText = '', rememberedSelectionContext = null) {
  const action = typeof item === 'string' ? item : item?.id;
  const actionTarget = typeof item === 'object' ? item.target : null;
  if (!action) return;

  switch (action) {
    case DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER:
      await openTargetInExplorer(actionTarget);
      break;
    case DESKTOP_CONTEXT_ACTIONS.LOCATE_FILE:
      await openTargetInExplorer({ path: actionTarget?.locatePath, kind: 'directory' });
      break;
    case DESKTOP_CONTEXT_ACTIONS.PIN_SESSION:
      dispatchSessionPinToggle(actionTarget, true);
      break;
    case DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION:
      dispatchSessionPinToggle(actionTarget, false);
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE:
      await copyTextWithToast(actionTarget?.title || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_ID:
      await copyTextWithToast(actionTarget?.sessionId || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_SESSION:
      try {
        const result = await api.exportSession(
          actionTarget?.sessionId || '',
          actionTarget?.workspaceHash || '',
        );
        if (result?.cancelled) break;
        if (result?.ok) {
          toast({ kind: 'ok', text: `会话已导出:${result.filename || 'session.md'}` });
        } else {
          toast({ kind: 'err', text: '导出会话失败:' + (result?.error || '') });
        }
      } catch (e) {
        const detail = e?.body?.error || e?.message || '';
        toast({ kind: 'err', text: '导出会话失败:' + detail });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_WORKSPACE_PATH:
      await copyTextWithToast(actionTarget?.path || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH:
      await copyTextWithToast(actionTarget?.relativePath || actionTarget?.file || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH:
      await copyTextWithToast(actionTarget?.absolutePath || actionTarget?.path || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_MESSAGE_TEXT:
      await copyTextWithToast(actionTarget?.text || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_VISIBLE_TOOL_OUTPUT:
      await copyTextWithToast(actionTarget?.visibleOutput || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_FULL_TOOL_OUTPUT:
      await copyTextWithToast(actionTarget?.fullOutput || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME:
      await copyTextWithToast(actionTarget?.name || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL:
      await copyTextWithToast(actionTarget?.url || actionTarget?.path || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE:
    case DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_IMAGE:
      await copyImageWithToast(actionTarget);
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_PNG:
      await exportMermaidWithToast(actionTarget, 'png', 'PNG 图片');
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SVG:
      await exportMermaidWithToast(actionTarget, 'svg', 'SVG');
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SOURCE:
      await exportMermaidWithToast(actionTarget, 'source', 'Mermaid 源码');
      break;
    case DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT:
      if (!dispatchDesktopContextAction(action, actionTarget, {
        selectedText: rememberedText,
        selectionContext: rememberedSelectionContext,
      })) {
        toast({ kind: 'err', text: '操作不可用' });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT:
    case DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT:
      if (!dispatchDesktopContextAction(action, actionTarget)) {
        toast({ kind: 'err', text: '操作不可用' });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.SELECT_ALL:
      selectAllForTarget(target);
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY:
      try {
        await copySelectionFromTarget(target, rememberedText);
        toast({ kind: 'ok', text: '已复制' });
      } catch (e) {
        toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
      }
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
      if (!dispatchDesktopContextAction(action, actionTarget)) {
        toast({ kind: 'err', text: '操作不可用' });
      }
      break;
  }
}

export function DesktopContextMenu() {
  const [menu, setMenuState] = useState(null);
  const menuRef = useRef(null);
  const reopenTimerRef = useRef(0);
  const targetRef = useRef(null);
  const lastSelectionRef = useRef({ target: null, text: '' });
  // webapp 兼容模式(Edge --app)没有 webview bridge,但定位仍是"桌面应用替身",
  // 自定义右键菜单照常启用;只有普通浏览器直连保留原生右键。
  const desktop = useMemo(() => isDesktopShell() || isWebappCompat(), []);

  const setMenu = useCallback((nextMenu) => {
    menuRef.current = nextMenu;
    setMenuState(nextMenu);
  }, []);

  const clearReopenTimer = useCallback(() => {
    if (!reopenTimerRef.current) return;
    window.clearTimeout(reopenTimerRef.current);
    reopenTimerRef.current = 0;
  }, []);

  const close = useCallback(() => {
    clearReopenTimer();
    setMenu(null);
  }, [clearReopenTimer, setMenu]);

  const openWithSwitchGap = useCallback((nextMenu) => {
    const delay = contextMenuOpenDelay({
      hasVisibleMenu: !!menuRef.current,
      hasPendingMenu: !!reopenTimerRef.current,
    });
    clearReopenTimer();
    if (delay <= 0) {
      setMenu(nextMenu);
      return;
    }
    setMenu(null);
    reopenTimerRef.current = window.setTimeout(() => {
      reopenTimerRef.current = 0;
      setMenu(nextMenu);
    }, delay);
  }, [clearReopenTimer, setMenu]);

  useEffect(() => {
    const rememberSelection = () => {
      const target = document.activeElement;
      const text = selectedTextForTarget(target);
      if (text) lastSelectionRef.current = { target: editableElementFrom(target) || target, text };
    };
    const closeFromPointer = (event) => {
      if (event.target instanceof Element && event.target.closest('.ace-desktop-context-menu')) return;
      close();
    };
    const onContextMenu = (event) => {
      // 控制台终端区由 ConsoleDock 自己处理右键(VS Code 式 复制/粘贴),这里放行
      // (不 preventDefault / 不 stopPropagation),让事件继续冒泡到终端的 onContextMenu。
      const rawTarget = event.target;
      if (rawTarget instanceof Element && rawTarget.closest('.ace-console-term')) return;

      const candidateTargets = contextTargetsFromElement(rawTarget);
      if (!shouldUseCustomContextMenu({
        desktop,
        mermaidTarget: candidateTargets.mermaidTarget,
      })) {
        close();
        return;
      }

      event.preventDefault();
      event.stopPropagation();

      const target = event.target;
      targetRef.current = target;
      const editableTarget = editableElementFrom(target);
      const editable = !!editableTarget;
      const contextTargets = editable ? {} : candidateTargets;
      const sessionPinTarget = contextTargets.sessionTarget
        ? {
            sessionId: contextTargets.sessionTarget.sessionId,
            workspaceHash: contextTargets.sessionTarget.workspaceHash,
            pinned: contextTargets.sessionTarget.pinned,
          }
        : null;
      let selectedText = selectedTextForTarget(target);
      if (!selectedText && editableTarget && lastSelectionRef.current.target === editableTarget) {
        selectedText = lastSelectionRef.current.text;
      }
      const hasSelection = selectedText.length > 0;
      const selectionContext = hasSelection
        ? selectionContextFromWindowSelection({ target, selectedText })
        : null;
      const debug = !!window.__ACECODE_DESKTOP_DEBUG__;
      const items = buildDesktopContextMenuItems({
        editable,
        hasSelection,
        debug,
        ...contextTargets,
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
      openWithSwitchGap({ ...pos, items, selectedText, selectionContext });
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
      clearReopenTimer();
    };
  }, [clearReopenTimer, close, desktop, openWithSwitchGap]);

  if (!menu) return null;

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
          key={typeof action === 'string' ? action : action.id}
          type="button"
          role="menuitem"
          disabled={typeof action === 'object' && action.enabled === false}
          className={[
            'ace-desktop-context-menu-item',
            typeof action === 'object' && action.separatorBefore ? 'ace-desktop-context-menu-separator' : '',
            typeof action === 'object' && action.danger ? 'ace-desktop-context-menu-danger' : '',
          ].filter(Boolean).join(' ')}
          onClick={async () => {
            if (typeof action === 'object' && action.enabled === false) return;
            if (typeof action === 'object' && action.confirm && !window.confirm(action.confirm)) return;
            const target = targetRef.current;
            const selectedText = menu.selectedText || '';
            const selectionContext = menu.selectionContext || null;
            close();
            await runAction(action, target, selectedText, selectionContext);
          }}
        >
          {ACTION_LABELS[typeof action === 'string' ? action : action.id] || (typeof action === 'string' ? action : action.id)}
        </button>
      ))}
    </div>
  );
}
