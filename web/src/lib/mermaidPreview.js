export const MERMAID_PREVIEW_EVENT = 'acecode:mermaid-preview';
export const MAX_MERMAID_PREVIEW_SOURCE_BYTES = 64 * 1024;
export const MAX_MERMAID_PREVIEW_SVG_BYTES = 4 * 1024 * 1024;
export const MAX_MERMAID_PREVIEW_DIMENSION = 32768;

export function mermaidPreviewCanvasColor(theme) {
  return theme === 'dark' ? '#333333' : '#ffffff';
}

function utf8ByteLength(value) {
  return new TextEncoder().encode(value).byteLength;
}

export function normalizeMermaidPreviewDetail(detail) {
  if (!detail || typeof detail.source !== 'string' || !detail.source.trim()
      || utf8ByteLength(detail.source) > MAX_MERMAID_PREVIEW_SOURCE_BYTES
      || typeof detail.svg !== 'string' || !detail.svg.trimStart().startsWith('<svg')) {
    return null;
  }
  if (utf8ByteLength(detail.svg) > MAX_MERMAID_PREVIEW_SVG_BYTES) return null;
  const width = Number(detail.width);
  const height = Number(detail.height);
  if (!Number.isFinite(width) || !Number.isFinite(height)
      || width <= 0 || height <= 0
      || width > MAX_MERMAID_PREVIEW_DIMENSION
      || height > MAX_MERMAID_PREVIEW_DIMENSION) {
    return null;
  }
  return {
    source: detail.source,
    svg: detail.svg,
    width,
    height,
    alt: typeof detail.alt === 'string' ? detail.alt : 'Mermaid diagram',
    theme: detail.theme === 'dark' ? 'dark' : 'light',
  };
}

export function dispatchMermaidPreview(win, detail) {
  const normalized = normalizeMermaidPreviewDetail(detail);
  const EventType = win?.CustomEvent || globalThis.CustomEvent;
  if (!normalized || typeof EventType !== 'function'
      || typeof win?.dispatchEvent !== 'function') {
    return false;
  }
  return win.dispatchEvent(new EventType(MERMAID_PREVIEW_EVENT, { detail: normalized }));
}
