import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('shared image lightbox owns bounded wheel zoom and captured left drag', () => {
  const component = source('components/ImageLightbox.jsx');
  assert.match(component, /viewport\.addEventListener\('wheel', handleWheel, options\)/);
  assert.match(component, /const options = \{ passive: false \}/);
  assert.match(component, /if \(event\.button !== 0\) return/);
  assert.match(component, /setPointerCapture\?\.\(event\.pointerId\)/);
  assert.match(component, /onPointerMove=\{movePan\}/);
  assert.match(component, /onPointerCancel=\{stopPan\}/);
  assert.match(component, /aria-label="缩小预览"/);
  assert.match(component, /aria-label="放大预览"/);
  assert.match(component, /aria-label="适应窗口"/);
  assert.match(component, /event\.key !== 'Escape'/);
});

run('shared image lightbox retains caller metadata and backdrop boundaries', () => {
  const component = source('components/ImageLightbox.jsx');
  assert.match(component, /\.\.\.contextMenuAttrs/);
  assert.match(component, /width=\{previewWidth\}/);
  assert.match(component, /height=\{previewHeight\}/);
  assert.match(component, /backgroundColor: previewCanvasColor/);
  assert.match(component, /registerMermaidExportTarget\(element, mermaidExport\)/);
  assert.match(component, /data-mermaid-export-target=\{mermaidExport \? 'true' : undefined\}/);
  assert.match(component, /onClick=\{\(event\) => event\.stopPropagation\(\)\}/);
  assert.match(component, /onClick=\{\(\) => onClose\?\.\(\)\}/);
  assert.match(component, /draggable="false"/);
});

run('Mermaid preview host is mounted once and owns its Blob URL lifetime', () => {
  const main = source('main.jsx');
  const host = source('components/MermaidPreviewHost.jsx');
  assert.match(main, /<MermaidPreviewHost \/>/);
  assert.match(host, /window\.addEventListener\(MERMAID_PREVIEW_EVENT, openPreview\)/);
  assert.match(host, /new window\.Blob\(/);
  assert.match(host, /window\.URL\.createObjectURL/);
  assert.match(host, /window\.URL\.revokeObjectURL/);
  assert.match(host, /canvasColor: mermaidPreviewCanvasColor\(detail\.theme\)/);
  assert.match(host, /mermaidExport: detail/);
  assert.match(host, /return <ImageLightbox preview=\{preview\} onClose=\{closePreview\} \/>/);
});

run('ready Mermaid images are native preview buttons backed by sanitized SVG detail', () => {
  const renderer = source('lib/mermaidRenderer.js');
  const styles = source('styles/globals.css');
  assert.match(renderer, /owner\.createElement\('button'\)/);
  assert.match(renderer, /previewTrigger\.type = 'button'/);
  assert.match(renderer, /dispatchMermaidPreview\(win, \{/);
  assert.match(renderer, /registerMermaidExportTarget\(previewTrigger, \{/);
  assert.match(renderer, /source,/);
  assert.match(renderer, /svg: result\.svg/);
  assert.match(renderer, /theme,/);
  assert.match(renderer, /target\.replaceChildren\(previewTrigger\)/);
  assert.match(styles, /\.ace-mermaid-preview-trigger/);
  assert.match(styles, /width: max-content/);
  assert.match(styles, /cursor: zoom-in/);
});

run('shared context menu intercepts ordinary browser right clicks only for Mermaid targets', () => {
  const component = source('components/DesktopContextMenu.jsx');
  assert.match(component, /const candidateTargets = contextTargetsFromElement\(rawTarget\)/);
  assert.match(component, /shouldUseCustomContextMenu\(\{/);
  assert.match(component, /mermaidTarget: candidateTargets\.mermaidTarget/);
  assert.match(component, /exportMermaidAsset\(target, format\)/);
  assert.doesNotMatch(component, /if \(!desktop\) return undefined/);
  assert.match(component, /if \(!menu\) return null/);
});
