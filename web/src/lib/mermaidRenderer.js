import mermaid from 'mermaid';
import { registerMermaidExportTarget } from './mermaidExport.js';
import { dispatchMermaidPreview } from './mermaidPreview.js';

export const MAX_MERMAID_SOURCE_BYTES = 64 * 1024;
export const MAX_MERMAID_SOURCE_LINES = 1000;
export const MAX_MERMAID_EDGES = 500;
export const MAX_MERMAID_SVG_BYTES = 4 * 1024 * 1024;
export const MAX_MERMAID_SVG_DIMENSION = 32768;

const SVG_NAMESPACE = 'http://www.w3.org/2000/svg';
const XLINK_NAMESPACE = 'http://www.w3.org/1999/xlink';
const DIAGRAM_SELECTOR = '[data-mermaid-diagram]';
const SOURCE_DIAGRAM_SELECTOR = '[data-mermaid-diagram="source"]';
const SOURCE_SELECTOR = '[data-mermaid-source="true"]';
const TARGET_SELECTOR = '[data-mermaid-render-target="true"]';
const SAFE_MERMAID_LABEL_BREAK = /<br[ \t]*\/?>/gi;

const ALLOWED_SVG_ELEMENTS = new Set([
  'circle',
  'clippath',
  'defs',
  'desc',
  'ellipse',
  'fecolormatrix',
  'fecomposite',
  'fedropshadow',
  'feflood',
  'fegaussianblur',
  'femerge',
  'femergenode',
  'feoffset',
  'filter',
  'g',
  'line',
  'lineargradient',
  'marker',
  'mask',
  'path',
  'pattern',
  'polygon',
  'polyline',
  'radialgradient',
  'rect',
  'stop',
  'style',
  'svg',
  'symbol',
  'text',
  'textpath',
  'title',
  'tspan',
]);

const FAMILY_HEADERS = [
  { family: 'flowchart', pattern: /^(?:flowchart|graph)\s+(?:TB|TD|BT|RL|LR)\s*;?(?:\s+%%.*)?$/ },
  { family: 'state', pattern: /^stateDiagram(?:-v2)?\s*;?(?:\s+%%.*)?$/ },
  { family: 'class', pattern: /^classDiagram\s*;?(?:\s+%%.*)?$/ },
  { family: 'er', pattern: /^erDiagram\s*;?(?:\s+%%.*)?$/ },
  { family: 'sequence', pattern: /^sequenceDiagram\s*;?(?:\s+%%.*)?$/ },
];

function utf8ByteLength(value) {
  return new TextEncoder().encode(String(value || '')).byteLength;
}

function rejected(reason) {
  return { ok: false, reason };
}

export function inspectMermaidSource(value) {
  const source = String(value || '').replace(/\r\n?/g, '\n');
  if (!source.trim()) return rejected('empty');
  if (utf8ByteLength(source) > MAX_MERMAID_SOURCE_BYTES) return rejected('too-large');

  const lines = source.split('\n');
  if (lines.length > MAX_MERMAID_SOURCE_LINES) return rejected('too-many-lines');
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(source)) {
    return rejected('control-character');
  }

  const firstMeaningful = lines.find((line) => {
    const trimmed = line.trim();
    return trimmed && !/^%%(?!\{)/.test(trimmed);
  })?.trim() || '';
  if (firstMeaningful === '---') return rejected('frontmatter');

  const family = FAMILY_HEADERS.find(({ pattern }) => pattern.test(firstMeaningful))?.family;
  if (!family) return rejected('unsupported-family');

  if (/%%\s*\{/.test(source)) return rejected('directive');
  if (/@\s*\{/.test(source)) return rejected('generalized-shape');
  const htmlSafetySource = source.replace(SAFE_MERMAID_LABEL_BREAK, '');
  if (/<\/?[A-Za-z][^>\r\n]*>/i.test(htmlSafetySource)
      || /&(?:lt|gt|#0*60|#x0*3c);/i.test(source)) {
    return rejected('html');
  }
  if (/!\s*\[/.test(source) || /\][ \t]*\(/.test(source)) return rejected('markdown-link');
  if (/\$\$/.test(source)) return rejected('math-label');
  if (/\bfa[blrs]?:fa-[\w-]+/i.test(source)) return rejected('icon-label');
  if (/(?:^|[^\w])(?:https?|ftp|file|data|blob|mailto|tel|about|chrome|resource|javascript|vbscript)\s*:/i.test(source)
      || /\/\/[^\s]/.test(source)
      || /url\s*\(/i.test(source)) {
    return rejected('external-resource');
  }
  if (/(?:expression\s*\(|-moz-binding\b|behavior\s*:|@(?:import|font-face|namespace|supports|media|document|page)\b)/i.test(source)) {
    return rejected('unsafe-style');
  }
  if (/(?:^|[\r\n;])\s*(?:click|href|links?|callback|call)\b/im.test(source)) {
    return rejected('interactive');
  }

  return { ok: true, family, source };
}

export function mermaidConfig(theme) {
  return {
    startOnLoad: false,
    securityLevel: 'strict',
    suppressErrorRendering: true,
    htmlLabels: false,
    theme: theme === 'dark' ? 'dark' : 'default',
    look: 'classic',
    layout: 'dagre',
    arrowMarkerAbsolute: false,
    deterministicIds: true,
    deterministicIDSeed: 'acecode',
    maxTextSize: MAX_MERMAID_SOURCE_BYTES,
    maxEdges: MAX_MERMAID_EDGES,
    logLevel: 'fatal',
  };
}

export function mermaidTheme(doc = globalThis.document) {
  return doc?.documentElement?.getAttribute?.('data-theme') === 'dark' ? 'dark' : 'light';
}

function isLocalFragmentUrl(value) {
  const text = String(value || '').trim();
  const unquoted = ((text.startsWith('"') && text.endsWith('"'))
    || (text.startsWith("'") && text.endsWith("'")))
    ? text.slice(1, -1).trim()
    : text;
  return /^#[A-Za-z_][\w:.-]*$/.test(unquoted);
}

export function isSafeMermaidResourceValue(value, { css = false } = {}) {
  const text = String(value || '');
  if (/\b(?:javascript|vbscript|data|blob|mailto|tel|about|chrome|resource|https?|ftp|file)\s*:/i.test(text)
      || /(?:^|[^:])\/\//.test(text)
      || /expression\s*\(|-moz-binding|behavior\s*:/i.test(text)) {
    return false;
  }

  let urlCount = 0;
  for (const match of text.matchAll(/url\s*\(([^)]*)\)/gi)) {
    urlCount += 1;
    if (!isLocalFragmentUrl(match[1])) return false;
  }
  const urlTokens = text.match(/url\s*\(/gi)?.length || 0;
  if (urlTokens !== urlCount) return false;

  if (css) {
    if (/@(?:import|font-face|namespace|supports|media|document|page)\b/i.test(text)) return false;
    const withoutKeyframes = text.replace(/@(?:-webkit-)?keyframes\s+[-\w]+/gi, '');
    if (withoutKeyframes.includes('@')) return false;
  }
  return true;
}

function numericDimension(value) {
  const match = String(value || '').trim().match(/^(?:\d+(?:\.\d+)?|\.\d+)(?:px)?$/i);
  if (!match) return null;
  const parsed = Number.parseFloat(match[0]);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
}

export function readMermaidSvgDimensions(root) {
  const viewBox = String(root?.getAttribute?.('viewBox') || '').trim();
  if (viewBox) {
    const values = viewBox.split(/[\s,]+/).map(Number);
    if (values.length !== 4 || values.some((value) => !Number.isFinite(value))
        || values[2] <= 0 || values[3] <= 0) {
      return null;
    }
    return { width: values[2], height: values[3] };
  }

  const width = numericDimension(root?.getAttribute?.('width'));
  const height = numericDimension(root?.getAttribute?.('height'));
  return width && height ? { width, height } : null;
}

function hasParserError(documentNode) {
  if (!documentNode?.documentElement) return true;
  try {
    return documentNode.getElementsByTagName('parsererror').length > 0;
  } catch {
    return true;
  }
}

export function sanitizeMermaidSvg(svg, {
  DOMParserType = globalThis.DOMParser,
  XMLSerializerType = globalThis.XMLSerializer,
} = {}) {
  const source = String(svg || '').trim();
  if (!source.startsWith('<svg') || utf8ByteLength(source) > MAX_MERMAID_SVG_BYTES
      || /<!DOCTYPE|<!ENTITY|<\?xml-stylesheet/i.test(source)
      || typeof DOMParserType !== 'function' || typeof XMLSerializerType !== 'function') {
    return null;
  }

  let documentNode;
  try {
    documentNode = new DOMParserType().parseFromString(source, 'image/svg+xml');
  } catch {
    return null;
  }
  if (hasParserError(documentNode)) return null;

  const root = documentNode.documentElement;
  if (String(root.localName || root.nodeName).toLowerCase() !== 'svg'
      || root.namespaceURI !== SVG_NAMESPACE) {
    return null;
  }

  let elements;
  try {
    elements = Array.from(documentNode.getElementsByTagName('*'));
  } catch {
    return null;
  }
  for (const element of elements) {
    const localName = String(element.localName || element.nodeName || '').toLowerCase();
    if (!ALLOWED_SVG_ELEMENTS.has(localName) || element.namespaceURI !== SVG_NAMESPACE) {
      return null;
    }

    if (localName === 'style'
        && !isSafeMermaidResourceValue(element.textContent, { css: true })) {
      return null;
    }

    for (const attribute of Array.from(element.attributes || [])) {
      const name = String(attribute.name || '').toLowerCase();
      const local = String(attribute.localName || name).toLowerCase();
      const value = String(attribute.value || '');
      if (name.startsWith('on') || local === 'href' || local === 'src'
          || local === 'base' || local === 'target') {
        return null;
      }
      if (name === 'xmlns') {
        if (value !== SVG_NAMESPACE) return null;
        continue;
      }
      if (name === 'xmlns:xlink') {
        if (value !== XLINK_NAMESPACE) return null;
        continue;
      }
      if (!isSafeMermaidResourceValue(value, { css: name === 'style' })) return null;
    }
  }

  const dimensions = readMermaidSvgDimensions(root);
  if (!dimensions || dimensions.width > MAX_MERMAID_SVG_DIMENSION
      || dimensions.height > MAX_MERMAID_SVG_DIMENSION) {
    return null;
  }

  let serialized;
  try {
    serialized = new XMLSerializerType().serializeToString(root);
  } catch {
    return null;
  }
  if (!serialized || utf8ByteLength(serialized) > MAX_MERMAID_SVG_BYTES) return null;
  return { svg: serialized, ...dimensions };
}

let nextRenderId = 1;

export function createMermaidRenderAdapter({
  runtime = mermaid,
  DOMParserType = globalThis.DOMParser,
  XMLSerializerType = globalThis.XMLSerializer,
} = {}) {
  let queue = Promise.resolve();

  const run = async (source, theme) => {
    const inspected = inspectMermaidSource(source);
    if (!inspected.ok) return null;
    try {
      runtime.initialize(mermaidConfig(theme));
      const parsed = await runtime.parse(inspected.source, { suppressErrors: true });
      if (parsed === false) return null;
      const id = `ace-mermaid-${nextRenderId++}`;
      const rendered = await runtime.render(id, inspected.source);
      return sanitizeMermaidSvg(rendered?.svg, { DOMParserType, XMLSerializerType });
    } catch {
      return null;
    }
  };

  return (source, theme) => {
    const pending = queue.then(() => run(source, theme), () => run(source, theme));
    queue = pending.then(() => undefined, () => undefined);
    return pending;
  };
}

export function installMermaidRenderer(
  win = globalThis.window,
  doc = globalThis.document,
  render = null,
) {
  const Observer = win?.MutationObserver || globalThis.MutationObserver;
  const BlobType = win?.Blob || globalThis.Blob;
  const urlApi = win?.URL || globalThis.URL;
  const DOMParserType = win?.DOMParser || globalThis.DOMParser;
  const XMLSerializerType = win?.XMLSerializer || globalThis.XMLSerializer;
  if (!doc?.documentElement || typeof Observer !== 'function'
      || typeof BlobType !== 'function' || typeof urlApi?.createObjectURL !== 'function'
      || typeof DOMParserType !== 'function' || typeof XMLSerializerType !== 'function') {
    return () => {};
  }

  const requestRender = typeof render === 'function'
    ? render
    : createMermaidRenderAdapter({ DOMParserType, XMLSerializerType });
  const jobs = new WeakMap();
  let stopped = false;
  let scheduled = false;

  const setState = (frame, state) => frame.setAttribute('data-mermaid-diagram', state);
  const invalidate = (frame) => {
    const active = jobs.get(frame);
    active?.release?.();
    jobs.set(frame, { token: {} });
  };
  const restoreSource = (frame) => {
    invalidate(frame);
    const target = frame.querySelector(TARGET_SELECTOR);
    target?.replaceChildren();
    target?.setAttribute('aria-busy', 'false');
    frame.removeAttribute('data-mermaid-theme');
    setState(frame, 'source');
  };

  const renderFrame = async (frame) => {
    if (stopped || frame.getAttribute('data-mermaid-diagram') !== 'source') return;
    const sourceNode = frame.querySelector(SOURCE_SELECTOR);
    const target = frame.querySelector(TARGET_SELECTOR);
    const source = String(sourceNode?.textContent || '');
    if (!target || !source.trim()) return;

    const token = {};
    const theme = mermaidTheme(doc);
    jobs.set(frame, { token, source, theme });
    setState(frame, 'loading');
    target.setAttribute('aria-busy', 'true');
    const result = await requestRender(source, theme);
    const active = jobs.get(frame);
    if (stopped || active?.token !== token || frame.isConnected === false) {
      return;
    }
    if (String(sourceNode.textContent || '') !== source || mermaidTheme(doc) !== theme) {
      restoreSource(frame);
      schedule();
      return;
    }
    if (!result) {
      target.setAttribute('aria-busy', 'false');
      setState(frame, 'error');
      return;
    }

    const owner = frame.ownerDocument || doc;
    const previewTrigger = owner.createElement('button');
    const image = owner.createElement('img');
    const objectUrl = urlApi.createObjectURL(
      new BlobType([result.svg], { type: 'image/svg+xml;charset=utf-8' }),
    );
    let released = false;
    const release = () => {
      if (released) return;
      released = true;
      try { urlApi.revokeObjectURL(objectUrl); } catch { /* best effort */ }
    };
    jobs.set(frame, { token, source, theme, release });
    previewTrigger.type = 'button';
    previewTrigger.className = 'ace-mermaid-preview-trigger';
    previewTrigger.title = '放大预览 Mermaid 图表';
    previewTrigger.setAttribute('aria-label', '放大预览 Mermaid 图表');
    registerMermaidExportTarget(previewTrigger, {
      source,
      svg: result.svg,
      width: result.width,
      height: result.height,
      theme,
    });
    previewTrigger.addEventListener('click', () => {
      dispatchMermaidPreview(win, {
        source,
        svg: result.svg,
        width: result.width,
        height: result.height,
        alt: image.alt,
        theme,
      });
    });
    image.className = 'ace-mermaid-svg';
    image.alt = 'Mermaid diagram';
    image.width = Math.ceil(result.width);
    image.height = Math.ceil(result.height);
    image.decoding = 'async';
    image.draggable = false;
    image.addEventListener('load', () => {
      release();
      if (stopped || jobs.get(frame)?.token !== token || frame.isConnected === false) return;
      if (String(sourceNode.textContent || '') !== source || mermaidTheme(doc) !== theme) {
        restoreSource(frame);
        schedule();
        return;
      }
      target.setAttribute('aria-busy', 'false');
      frame.setAttribute('data-mermaid-theme', theme);
      setState(frame, 'ready');
    }, { once: true });
    image.addEventListener('error', () => {
      release();
      if (stopped || jobs.get(frame)?.token !== token || frame.isConnected === false) return;
      if (String(sourceNode.textContent || '') !== source || mermaidTheme(doc) !== theme) {
        restoreSource(frame);
        schedule();
        return;
      }
      target.replaceChildren();
      target.setAttribute('aria-busy', 'false');
      setState(frame, 'error');
    }, { once: true });
    image.src = objectUrl;
    previewTrigger.append(image);
    target.replaceChildren(previewTrigger);
  };

  const scan = () => {
    scheduled = false;
    if (stopped) return;
    for (const frame of doc.querySelectorAll(SOURCE_DIAGRAM_SELECTOR)) renderFrame(frame);
  };
  const schedule = () => {
    if (stopped || scheduled) return;
    scheduled = true;
    const enqueue = win?.queueMicrotask || globalThis.queueMicrotask;
    if (typeof enqueue === 'function') enqueue(scan);
    else Promise.resolve().then(scan);
  };
  const resetForTheme = () => {
    for (const frame of doc.querySelectorAll(DIAGRAM_SELECTOR)) restoreSource(frame);
  };

  const observer = new Observer((records) => {
    if (records.some((record) => record.type === 'attributes'
        && record.attributeName === 'data-theme')) {
      resetForTheme();
    }
    schedule();
  });
  observer.observe(doc.documentElement, {
    attributes: true,
    attributeFilter: ['data-theme'],
    childList: true,
    subtree: true,
  });
  schedule();

  return () => {
    stopped = true;
    observer.disconnect();
    for (const frame of doc.querySelectorAll(DIAGRAM_SELECTOR)) invalidate(frame);
  };
}
