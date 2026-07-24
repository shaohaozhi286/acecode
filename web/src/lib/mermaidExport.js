export const MERMAID_EXPORT_TARGET_SELECTOR = '[data-mermaid-export-target="true"]';
export const MAX_MERMAID_EXPORT_SOURCE_BYTES = 64 * 1024;
export const MAX_MERMAID_EXPORT_SVG_BYTES = 4 * 1024 * 1024;
export const MAX_MERMAID_EXPORT_DIMENSION = 32768;
export const MERMAID_PNG_PREFERRED_SCALE = 2;
export const MERMAID_PNG_MAX_DIMENSION = 8192;
export const MERMAID_PNG_MAX_PIXELS = 16 * 1024 * 1024;

const exportTargets = new WeakMap();

function utf8ByteLength(value) {
  return new TextEncoder().encode(String(value || '')).byteLength;
}

function errorMessage(error, fallback = 'export unavailable') {
  return String(error?.message || error || fallback);
}

function failed(error) {
  return { ok: false, error: errorMessage(error) };
}

export function normalizeMermaidExportDetail(detail) {
  if (!detail || typeof detail !== 'object') return null;
  const source = typeof detail.source === 'string' ? detail.source : '';
  const svg = typeof detail.svg === 'string' ? detail.svg : '';
  const width = Number(detail.width);
  const height = Number(detail.height);
  if (!source.trim() || utf8ByteLength(source) > MAX_MERMAID_EXPORT_SOURCE_BYTES
      || !svg.trimStart().startsWith('<svg')
      || utf8ByteLength(svg) > MAX_MERMAID_EXPORT_SVG_BYTES
      || !Number.isFinite(width) || !Number.isFinite(height)
      || width <= 0 || height <= 0
      || width > MAX_MERMAID_EXPORT_DIMENSION
      || height > MAX_MERMAID_EXPORT_DIMENSION) {
    return null;
  }
  return Object.freeze({
    type: 'mermaid',
    source,
    svg,
    width,
    height,
    theme: detail.theme === 'dark' ? 'dark' : 'light',
  });
}

function isWeakMapKey(value) {
  return (typeof value === 'object' && value !== null) || typeof value === 'function';
}

export function unregisterMermaidExportTarget(element) {
  if (!isWeakMapKey(element)) return;
  exportTargets.delete(element);
  try {
    element.removeAttribute?.('data-mermaid-export-target');
  } catch {
    // A detached host may no longer expose mutable attributes.
  }
}

export function registerMermaidExportTarget(element, detail) {
  if (!isWeakMapKey(element)) return null;
  const normalized = normalizeMermaidExportDetail(detail);
  if (!normalized) {
    unregisterMermaidExportTarget(element);
    return null;
  }
  exportTargets.set(element, normalized);
  try {
    element.setAttribute?.('data-mermaid-export-target', 'true');
  } catch {
    exportTargets.delete(element);
    return null;
  }
  return normalized;
}

export function mermaidExportTargetFromElement(target) {
  const element = target?.closest?.(MERMAID_EXPORT_TARGET_SELECTOR);
  return isWeakMapKey(element) ? exportTargets.get(element) || null : null;
}

export function mermaidFamilyFromSource(source) {
  const header = String(source || '').split(/\r?\n/g).find((line) => {
    const trimmed = line.trim();
    return trimmed && !trimmed.startsWith('%%');
  })?.trim() || '';
  if (/^(?:flowchart|graph)\b/i.test(header)) return 'flowchart';
  if (/^stateDiagram(?:-v2)?\b/i.test(header)) return 'state';
  if (/^classDiagram\b/i.test(header)) return 'class';
  if (/^erDiagram\b/i.test(header)) return 'er';
  if (/^sequenceDiagram\b/i.test(header)) return 'sequence';
  return 'diagram';
}

export function mermaidExportFilename(detail, format) {
  const family = mermaidFamilyFromSource(detail?.source);
  const extension = format === 'source' ? 'mmd' : String(format || '').toLowerCase();
  return `mermaid-${family}.${extension || 'txt'}`;
}

export function mermaidPngDimensions(width, height, {
  preferredScale = MERMAID_PNG_PREFERRED_SCALE,
  maxDimension = MERMAID_PNG_MAX_DIMENSION,
  maxPixels = MERMAID_PNG_MAX_PIXELS,
} = {}) {
  const logicalWidth = Number(width);
  const logicalHeight = Number(height);
  const preferred = Number(preferredScale);
  const dimensionLimit = Number(maxDimension);
  const pixelLimit = Number(maxPixels);
  if (!Number.isFinite(logicalWidth) || !Number.isFinite(logicalHeight)
      || !Number.isFinite(preferred) || !Number.isFinite(dimensionLimit)
      || !Number.isFinite(pixelLimit) || logicalWidth <= 0 || logicalHeight <= 0
      || preferred <= 0 || dimensionLimit < 1 || pixelLimit < 1) {
    return null;
  }

  const scale = Math.min(
    preferred,
    dimensionLimit / logicalWidth,
    dimensionLimit / logicalHeight,
    Math.sqrt(pixelLimit / (logicalWidth * logicalHeight)),
  );
  if (!Number.isFinite(scale) || scale <= 0) return null;
  const outputWidth = Math.max(1, Math.floor(logicalWidth * scale));
  const outputHeight = Math.max(1, Math.floor(logicalHeight * scale));
  return {
    width: outputWidth,
    height: outputHeight,
    scale: Math.min(outputWidth / logicalWidth, outputHeight / logicalHeight),
  };
}

function revokeObjectUrl(urlApi, url) {
  if (!url) return;
  try {
    urlApi?.revokeObjectURL?.(url);
  } catch {
    // Object URL cleanup is best effort during document teardown.
  }
}

export function downloadMermaidBlob(blob, filename, {
  document: doc = globalThis.document,
  URLApi = globalThis.URL,
  schedule = globalThis.setTimeout?.bind(globalThis),
} = {}) {
  if (!blob || !filename || typeof doc?.createElement !== 'function'
      || typeof URLApi?.createObjectURL !== 'function') {
    return failed('download unavailable');
  }

  let objectUrl = '';
  let anchor = null;
  try {
    objectUrl = URLApi.createObjectURL(blob);
    if (!objectUrl) return failed('download URL unavailable');
    anchor = doc.createElement('a');
    anchor.href = objectUrl;
    anchor.download = filename;
    anchor.style.display = 'none';
    (doc.body || doc.documentElement)?.append?.(anchor);
    anchor.click();
    anchor.remove?.();
    anchor = null;
    const cleanup = () => revokeObjectUrl(URLApi, objectUrl);
    if (typeof schedule === 'function') schedule(cleanup, 0);
    else cleanup();
    return { ok: true, filename };
  } catch (error) {
    try { anchor?.remove?.(); } catch { /* best effort */ }
    revokeObjectUrl(URLApi, objectUrl);
    return failed(error);
  }
}

function blobFor(text, type, BlobType) {
  if (typeof BlobType !== 'function') return null;
  try {
    return new BlobType([text], { type });
  } catch {
    return null;
  }
}

export function exportMermaidSvg(detail, {
  BlobType = globalThis.Blob,
  ...downloadOptions
} = {}) {
  const payload = normalizeMermaidExportDetail(detail);
  if (!payload) return failed('invalid Mermaid export payload');
  const blob = blobFor(payload.svg, 'image/svg+xml;charset=utf-8', BlobType);
  if (!blob) return failed('SVG Blob unavailable');
  return downloadMermaidBlob(blob, mermaidExportFilename(payload, 'svg'), downloadOptions);
}

export function exportMermaidSource(detail, {
  BlobType = globalThis.Blob,
  ...downloadOptions
} = {}) {
  const payload = normalizeMermaidExportDetail(detail);
  if (!payload) return failed('invalid Mermaid export payload');
  const blob = blobFor(payload.source, 'text/plain;charset=utf-8', BlobType);
  if (!blob) return failed('source Blob unavailable');
  return downloadMermaidBlob(blob, mermaidExportFilename(payload, 'source'), downloadOptions);
}

function loadImage(objectUrl, ImageType) {
  return new Promise((resolve, reject) => {
    if (typeof ImageType !== 'function') {
      reject(new Error('image decoder unavailable'));
      return;
    }
    let image;
    try {
      image = new ImageType();
    } catch (error) {
      reject(error);
      return;
    }
    image.decoding = 'async';
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error('SVG image decode failed'));
    try {
      image.src = objectUrl;
    } catch (error) {
      reject(error);
    }
  });
}

function canvasPngBlob(canvas) {
  return new Promise((resolve, reject) => {
    if (typeof canvas?.toBlob !== 'function') {
      reject(new Error('PNG encoder unavailable'));
      return;
    }
    try {
      canvas.toBlob((blob) => {
        if (blob) resolve(blob);
        else reject(new Error('PNG encoding failed'));
      }, 'image/png');
    } catch (error) {
      reject(error);
    }
  });
}

export async function exportMermaidPng(detail, {
  document: doc = globalThis.document,
  URLApi = globalThis.URL,
  BlobType = globalThis.Blob,
  ImageType = globalThis.Image,
  schedule = globalThis.setTimeout?.bind(globalThis),
  rasterLimits,
} = {}) {
  const payload = normalizeMermaidExportDetail(detail);
  if (!payload) return failed('invalid Mermaid export payload');
  if (typeof doc?.createElement !== 'function'
      || typeof URLApi?.createObjectURL !== 'function') {
    return failed('PNG export unavailable');
  }
  const dimensions = mermaidPngDimensions(payload.width, payload.height, rasterLimits);
  if (!dimensions) return failed('invalid PNG dimensions');
  const svgBlob = blobFor(payload.svg, 'image/svg+xml;charset=utf-8', BlobType);
  if (!svgBlob) return failed('SVG Blob unavailable');

  let svgUrl = '';
  try {
    svgUrl = URLApi.createObjectURL(svgBlob);
    if (!svgUrl) return failed('SVG decode URL unavailable');
    const image = await loadImage(svgUrl, ImageType);
    const canvas = doc.createElement('canvas');
    canvas.width = dimensions.width;
    canvas.height = dimensions.height;
    const context = canvas.getContext?.('2d');
    if (!context) return failed('canvas unavailable');
    context.fillStyle = payload.theme === 'dark' ? '#333333' : '#ffffff';
    context.fillRect(0, 0, dimensions.width, dimensions.height);
    context.drawImage(image, 0, 0, dimensions.width, dimensions.height);
    const pngBlob = await canvasPngBlob(canvas);
    return downloadMermaidBlob(pngBlob, mermaidExportFilename(payload, 'png'), {
      document: doc,
      URLApi,
      schedule,
    });
  } catch (error) {
    return failed(error);
  } finally {
    revokeObjectUrl(URLApi, svgUrl);
  }
}

export async function exportMermaidAsset(detail, format, options) {
  if (format === 'png') return exportMermaidPng(detail, options);
  if (format === 'svg') return exportMermaidSvg(detail, options);
  if (format === 'source') return exportMermaidSource(detail, options);
  return failed('unsupported Mermaid export format');
}
