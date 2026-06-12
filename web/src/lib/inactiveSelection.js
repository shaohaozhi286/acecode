// CSS Custom Highlight API: 选区失焦后保留为暗淡高亮，新选区出现时清除。

let savedRanges = [];
let activeHighlight = null;

function isEditable(sel) {
  if (!sel || sel.rangeCount === 0) return false;
  const node = sel.anchorNode;
  if (!node) return false;
  const el = node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement;
  if (!el) return false;
  const tag = el.tagName;
  return tag === 'INPUT' || tag === 'TEXTAREA' || el.isContentEditable;
}

function clearHighlight() {
  if (activeHighlight) {
    CSS.highlights.delete('ace-inactive-selection');
    activeHighlight = null;
  }
}

function onSelectionChange() {
  const sel = window.getSelection();
  const hasContent = sel && sel.rangeCount > 0 && !sel.isCollapsed;

  if (hasContent) {
    clearHighlight();
    if (isEditable(sel)) {
      savedRanges = [];
      return;
    }
    savedRanges = [];
    for (let i = 0; i < sel.rangeCount; i++) {
      savedRanges.push(sel.getRangeAt(i).cloneRange());
    }
  } else if (savedRanges.length > 0) {
    try {
      activeHighlight = new Highlight(...savedRanges);
      CSS.highlights.set('ace-inactive-selection', activeHighlight);
    } catch {
      // ranges detached
    }
    savedRanges = [];
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
