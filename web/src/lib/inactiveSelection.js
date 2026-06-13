// CSS Custom Highlight API: 文件预览区选区失焦后保留为暗淡高亮，新选区出现时清除。

const PREVIEW_SELECTOR = '.ace-side-preview-code';

let savedRanges = [];
let activeHighlight = null;

function isInPreview(sel) {
  if (!sel || sel.rangeCount === 0) return false;
  const node = sel.anchorNode;
  if (!node) return false;
  const el = node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement;
  return !!el?.closest?.(PREVIEW_SELECTOR);
}

function clearHighlight() {
  if (activeHighlight) {
    CSS.highlights.delete('ace-inactive-selection');
    activeHighlight = null;
  }
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
  document.addEventListener('selectionchange', onSelectionChange);
  return () => {
    document.removeEventListener('selectionchange', onSelectionChange);
    clearHighlight();
    savedRanges = [];
  };
}
