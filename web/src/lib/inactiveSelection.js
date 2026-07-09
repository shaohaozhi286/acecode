import { unwrapMarks, wrapTextNodeRange } from './domTextMarks.js';

export const PREVIEW_SELECTOR = '.ace-side-preview-code';

const INACTIVE_SELECTION_MARK = 'ace-inactive-selection-mark';

let savedRanges = [];
let activeMarks = [];
let activeDocument = null;

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

function docForRange(range) {
  return (
    range?.startContainer?.ownerDocument ||
    range?.endContainer?.ownerDocument ||
    range?.commonAncestorContainer?.ownerDocument ||
    globalThis.document ||
    null
  );
}

function rootForRange(range) {
  return (
    previewElementFromTarget(range?.commonAncestorContainer) ||
    previewElementFromTarget(range?.startContainer) ||
    previewElementFromTarget(range?.endContainer)
  );
}

function rangeIntersectsTextNode(range, node) {
  if (!range || !node) return false;
  if (typeof range.intersectsNode === 'function') {
    try {
      return range.intersectsNode(node);
    } catch {
      return false;
    }
  }
  const doc = node.ownerDocument || globalThis.document;
  if (!doc?.createRange || typeof range.compareBoundaryPoints !== 'function') return false;
  const nodeRange = doc.createRange();
  nodeRange.selectNodeContents(node);
  try {
    return (
      range.compareBoundaryPoints(3, nodeRange) > 0 &&
      range.compareBoundaryPoints(1, nodeRange) < 0
    );
  } catch {
    return false;
  } finally {
    nodeRange.detach?.();
  }
}

function textPartsForRange(range) {
  const root = rootForRange(range);
  const doc = docForRange(range);
  if (!root || !doc?.createTreeWalker) return [];
  const view = doc.defaultView || globalThis.window || {};
  const nodeFilter = view.NodeFilter || globalThis.NodeFilter || {};
  const walker = doc.createTreeWalker(
    root,
    nodeFilter.SHOW_TEXT || 4,
    {
      acceptNode(node) {
        if (!String(node.nodeValue || '')) return nodeFilter.FILTER_REJECT || 2;
        return rangeIntersectsTextNode(range, node)
          ? (nodeFilter.FILTER_ACCEPT || 1)
          : (nodeFilter.FILTER_REJECT || 2);
      },
    },
  );
  const parts = [];
  let node = walker.nextNode();
  while (node) {
    const length = String(node.nodeValue || '').length;
    const start = range.startContainer === node ? range.startOffset : 0;
    const end = range.endContainer === node ? range.endOffset : length;
    if (end > start) {
      parts.push({ node, start, end });
    }
    node = walker.nextNode();
  }
  return parts;
}

function clearHighlight() {
  const doc = activeDocument || globalThis.document;
  unwrapMarks(doc?.body || doc, INACTIVE_SELECTION_MARK);
  activeMarks = [];
  activeDocument = null;
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
  globalThis.window?.getSelection?.()?.removeAllRanges?.();
}

function onPreviewMouseDown(event) {
  if (!shouldClearPreviewSelectionOnMouseDown(event, {
    hasSavedRanges: savedRanges.length > 0,
    hasActiveHighlight: activeMarks.length > 0,
    selection: globalThis.window?.getSelection?.(),
  })) {
    return;
  }
  clearSelection();
}

function promote() {
  if (savedRanges.length === 0) return;
  clearHighlight();

  const marks = [];
  for (const range of savedRanges) {
    try {
      const parts = textPartsForRange(range);
      for (const part of parts.reverse()) {
        const mark = wrapTextNodeRange(part.node, part.start, part.end, INACTIVE_SELECTION_MARK);
        if (mark) marks.push(mark);
      }
    } catch {
      // Ignore detached ranges from a preview that rerendered before selectionchange.
    }
  }

  activeMarks = marks;
  activeDocument = marks[0]?.ownerDocument || docForRange(savedRanges[0]) || null;
  savedRanges = [];
}

function onSelectionChange() {
  const sel = globalThis.window?.getSelection?.();
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
  if (!globalThis.document?.addEventListener || !globalThis.window?.getSelection) return () => {};
  document.addEventListener('mousedown', onPreviewMouseDown, true);
  document.addEventListener('selectionchange', onSelectionChange);
  return () => {
    document.removeEventListener('mousedown', onPreviewMouseDown, true);
    document.removeEventListener('selectionchange', onSelectionChange);
    clearHighlight();
    savedRanges = [];
  };
}
