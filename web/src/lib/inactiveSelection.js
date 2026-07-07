// CSS Custom Highlight API: 文件预览区选区失焦后保留为暗淡高亮，新选区出现时清除。

export const PREVIEW_SELECTOR = '.ace-side-preview-code';

let savedRanges = [];
let activeHighlight = null;

function elementFromNode(node) {
  if (!node) return null;
  if (node.nodeType === 1) return node;
  return node.parentElement || null;
}

export function previewElementFromTarget(target, selector = PREVIEW_SELECTOR) {
  const el = elementFromNode(target);
  return el && typeof el.closest === 'function'
    ? el.closest(selector)
    : null;
}

function isInPreview(sel) {
  if (!sel || sel.rangeCount === 0) return false;
  return !!previewElementFromTarget(sel.anchorNode);
}

function clearHighlight() {
  if (activeHighlight) {
    CSS.highlights.delete('ace-inactive-selection');
    activeHighlight = null;
  }
}

export function shouldClearPreviewSelectionOnMouseDown(event, {
  hasSavedRanges = false,
  hasActiveHighlight = false,
  selection = null,
} = {}) {
  if ((event?.button ?? 0) !== 0) return false;
  if (!previewElementFromTarget(event?.target)) return false;
  return hasSavedRanges || hasActiveHighlight || isInPreview(selection);
}

function clearSelection() {
  savedRanges = [];
  clearHighlight();
  window.getSelection?.()?.removeAllRanges?.();
}

function onPreviewMouseDown(event) {
  if (!shouldClearPreviewSelectionOnMouseDown(event, {
    hasSavedRanges: savedRanges.length > 0,
    hasActiveHighlight: !!activeHighlight,
    selection: window.getSelection?.(),
  })) {
    return;
  }
  clearSelection();
}

function promote() {
  if (savedRanges.length === 0) return;
  try {
    activeHighlight = new Highlight(...savedRanges);
    CSS.highlights.set('ace-inactive-selection', activeHighlight);
  } catch { /* ranges detached */ }
  savedRanges = [];
}

function onSelectionChange() {
  const sel = window.getSelection();
  const hasContent = sel && sel.rangeCount > 0 && !sel.isCollapsed;

  if (hasContent && isInPreview(sel)) {
    clearHighlight();
    savedRanges = [];
    for (let i = 0; i < sel.rangeCount; i++) {
      savedRanges.push(sel.getRangeAt(i).cloneRange());
    }
  } else {
    promote();
  }
}

export function initInactiveSelection() {
  if (typeof CSS === 'undefined' || !CSS.highlights) return () => {};
  document.addEventListener('mousedown', onPreviewMouseDown, true);
  document.addEventListener('selectionchange', onSelectionChange);
  return () => {
    document.removeEventListener('mousedown', onPreviewMouseDown, true);
    document.removeEventListener('selectionchange', onSelectionChange);
    clearHighlight();
    savedRanges = [];
  };
}
