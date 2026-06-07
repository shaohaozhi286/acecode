export const SELECTION_CONTEXT_TYPE = 'selection';
export const SELECTION_PREVIEW_SELECTOR = '[data-desktop-preview-path]';
export const MAX_SELECTION_CONTEXT_CHARS = 40000;

function asString(value) {
  return value == null ? '' : String(value);
}

function positiveInt(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? Math.floor(number) : 0;
}

export function selectionLineCount(text) {
  const value = asString(text);
  if (!value) return 0;
  return value.split(/\r\n|\r|\n/).length;
}

export function truncateSelectionText(text, limit = MAX_SELECTION_CONTEXT_CHARS) {
  const value = asString(text);
  const max = Math.max(0, Number(limit) || 0);
  if (!max || value.length <= max) return value;
  return `${value.slice(0, max)}\n[Selection truncated]`;
}

export function basenameForPath(path) {
  const value = asString(path).replace(/\\/g, '/');
  const parts = value.split('/').filter(Boolean);
  return parts.length ? parts[parts.length - 1] : value;
}

export function formatSelectionLineRange(source = {}) {
  const start = positiveInt(source.start_line ?? source.startLine);
  const end = positiveInt(source.end_line ?? source.endLine);
  if (!start) return '';
  if (!end || end === start) return String(start);
  return `${start}-${end}`;
}

export function formatSelectionContextLabel(ctx = {}) {
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const file = basenameForPath(source.path || ctx.path || ctx.label || '') || 'Selection';
  const range = formatSelectionLineRange(source);
  return range ? `${file}:${range}` : file;
}

export function formatSelectionContextNote(ctx = {}) {
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const lines = positiveInt(source.line_count ?? source.lineCount) || selectionLineCount(ctx.text);
  return lines > 0 ? `${lines} 行` : '';
}

export function selectionContextFingerprint(ctx = {}) {
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  return [
    SELECTION_CONTEXT_TYPE,
    source.path || ctx.path || '',
    source.start_line || source.startLine || '',
    source.end_line || source.endLine || '',
    asString(ctx.text),
  ].join('\u001f');
}

export function selectionContextLocationKey(ctx = {}) {
  if ((ctx.type || '') !== SELECTION_CONTEXT_TYPE) return '';
  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const path = asString(source.path || ctx.path || '').replace(/\\/g, '/');
  const start = positiveInt(source.start_line ?? source.startLine);
  const end = positiveInt(source.end_line ?? source.endLine) || start;
  if (!path || !start) return '';
  return [
    SELECTION_CONTEXT_TYPE,
    path,
    start,
    end,
  ].join('\u001f');
}

export function createSelectionContext({
  id = '',
  localId = '',
  text = '',
  path = '',
  kind = '',
  startLine = 0,
  endLine = 0,
  lineCount = 0,
} = {}) {
  const clippedText = truncateSelectionText(text);
  const safeStart = positiveInt(startLine);
  const safeEnd = positiveInt(endLine);
  const safeLineCount = positiveInt(lineCount) || selectionLineCount(clippedText);
  const source = {
    path: asString(path),
    kind: asString(kind),
    line_count: safeLineCount,
  };
  if (safeStart) source.start_line = safeStart;
  if (safeEnd || safeStart) source.end_line = safeEnd || safeStart;

  const context = {
    type: SELECTION_CONTEXT_TYPE,
    local_id: localId || id || '',
    id: id || localId || '',
    text: clippedText,
    source,
  };
  context.label = formatSelectionContextLabel(context);
  context.note = formatSelectionContextNote(context);
  return context;
}

export function normalizeComposerContext(ctx = {}) {
  if (!ctx || typeof ctx !== 'object') return null;
  if ((ctx.type || '') !== SELECTION_CONTEXT_TYPE) {
    return {
      type: ctx.type || 'browser',
      label: ctx.label || 'Browser',
      note: ctx.note || '',
    };
  }

  const source = ctx.source && typeof ctx.source === 'object' ? ctx.source : {};
  const normalized = createSelectionContext({
    id: ctx.id || ctx.local_id || '',
    localId: ctx.local_id || ctx.id || '',
    text: ctx.text || '',
    path: source.path || ctx.path || '',
    kind: source.kind || ctx.kind || '',
    startLine: source.start_line ?? source.startLine,
    endLine: source.end_line ?? source.endLine,
    lineCount: source.line_count ?? source.lineCount,
  });
  if (!normalized.text.trim()) return null;
  return {
    type: SELECTION_CONTEXT_TYPE,
    id: normalized.id || normalized.local_id || selectionContextFingerprint(normalized),
    label: normalized.label,
    note: normalized.note,
    text: normalized.text,
    source: normalized.source,
  };
}

export function contextPresentation(ctx = {}) {
  if ((ctx.type || '') === SELECTION_CONTEXT_TYPE) {
    const label = formatSelectionContextLabel(ctx);
    const note = formatSelectionContextNote(ctx);
    return {
      icon: 'info',
      label,
      note,
      title: [label, note].filter(Boolean).join('\n'),
      removeLabel: '移除引用上下文',
    };
  }
  return {
    icon: 'search',
    label: ctx.label || '浏览器',
    note: ctx.note || '',
    title: ctx.label || '浏览器',
    removeLabel: '移除浏览器上下文',
  };
}

function elementFromNode(node) {
  if (!node) return null;
  if (node.nodeType === 1) return node;
  return node.parentElement || null;
}

function closestPreviewElement(node) {
  const el = elementFromNode(node);
  return el && typeof el.closest === 'function'
    ? el.closest(SELECTION_PREVIEW_SELECTOR)
    : null;
}

function sourceElementForPreview(preview) {
  return preview?.querySelector?.('.ace-preview')
    || preview?.querySelector?.('.ace-side-markdown-preview')
    || preview;
}

function lineNumberAt(source, node, offset) {
  if (!source || !node) return 0;
  const nodeElement = elementFromNode(node);
  if (nodeElement && !source.contains(nodeElement) && source !== nodeElement) return 0;
  try {
    const range = document.createRange();
    range.selectNodeContents(source);
    range.setEnd(node, offset);
    const before = range.toString();
    range.detach?.();
    return before ? selectionLineCount(before) : 1;
  } catch {
    return 0;
  }
}

export function selectionContextFromWindowSelection({
  target = null,
  selectedText = '',
  id = '',
  localId = '',
} = {}) {
  if (typeof window === 'undefined' || typeof document === 'undefined') return null;
  const selection = window.getSelection?.();
  const text = selectedText || selection?.toString?.() || '';
  if (!text.trim()) return null;

  let preview = closestPreviewElement(target);
  let range = null;
  if (selection?.rangeCount) {
    range = selection.getRangeAt(0);
    const startPreview = closestPreviewElement(range.startContainer);
    const endPreview = closestPreviewElement(range.endContainer);
    if (startPreview && startPreview === endPreview) {
      preview = startPreview;
    }
  }
  if (!preview) return null;

  const path = preview.getAttribute('data-desktop-preview-path') || '';
  if (!path) return null;
  const kind = preview.getAttribute('data-desktop-preview-kind') || '';
  if (kind === 'image') return null;

  let startLine = 0;
  let endLine = 0;
  const source = sourceElementForPreview(preview);
  if (range && source) {
    startLine = lineNumberAt(source, range.startContainer, range.startOffset);
    endLine = lineNumberAt(source, range.endContainer, range.endOffset);
    if (startLine && endLine && endLine < startLine) {
      [startLine, endLine] = [endLine, startLine];
    }
  }

  return createSelectionContext({
    id,
    localId,
    text,
    path,
    kind,
    startLine,
    endLine,
    lineCount: selectionLineCount(text),
  });
}
