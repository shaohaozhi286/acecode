import assert from 'node:assert/strict';

import {
  dispatchMermaidPreview,
  MAX_MERMAID_PREVIEW_DIMENSION,
  MAX_MERMAID_PREVIEW_SOURCE_BYTES,
  MAX_MERMAID_PREVIEW_SVG_BYTES,
  MERMAID_PREVIEW_EVENT,
  mermaidPreviewCanvasColor,
  normalizeMermaidPreviewDetail,
} from './mermaidPreview.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const previewDetail = {
  source: 'flowchart TD\nA --> B',
  svg: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 80"></svg>',
  width: 160,
  height: 80,
  alt: 'Mermaid diagram',
  theme: 'light',
};

run('Mermaid preview detail accepts bounded sanitized output shape', () => {
  assert.deepEqual(normalizeMermaidPreviewDetail(previewDetail), previewDetail);
  assert.equal(normalizeMermaidPreviewDetail({ ...previewDetail, svg: '<div></div>' }), null);
  assert.equal(normalizeMermaidPreviewDetail({
    ...previewDetail,
    width: MAX_MERMAID_PREVIEW_DIMENSION + 1,
  }), null);
  assert.equal(normalizeMermaidPreviewDetail({ ...previewDetail, height: 0 }), null);
  assert.equal(normalizeMermaidPreviewDetail({ ...previewDetail, source: '' }), null);
  assert.equal(normalizeMermaidPreviewDetail({
    ...previewDetail,
    source: `flowchart TD\nA[${'x'.repeat(MAX_MERMAID_PREVIEW_SOURCE_BYTES)}]`,
  }), null);
  assert.equal(normalizeMermaidPreviewDetail({
    ...previewDetail,
    svg: `<svg>${'x'.repeat(MAX_MERMAID_PREVIEW_SVG_BYTES)}</svg>`,
  }), null);
});

run('Mermaid preview canvas follows the rendered Mermaid theme', () => {
  assert.equal(mermaidPreviewCanvasColor('light'), '#ffffff');
  assert.equal(mermaidPreviewCanvasColor('dark'), '#333333');
  assert.equal(normalizeMermaidPreviewDetail({ ...previewDetail, theme: 'dark' }).theme, 'dark');
  assert.equal(normalizeMermaidPreviewDetail({ ...previewDetail, theme: 'unknown' }).theme, 'light');
});

run('Mermaid preview dispatch uses one internal custom event', () => {
  const events = [];
  class FakeCustomEvent {
    constructor(type, init) {
      this.type = type;
      this.detail = init.detail;
    }
  }
  const win = {
    CustomEvent: FakeCustomEvent,
    dispatchEvent(event) {
      events.push(event);
      return true;
    },
  };
  assert.equal(dispatchMermaidPreview(win, previewDetail), true);
  assert.equal(events.length, 1);
  assert.equal(events[0].type, MERMAID_PREVIEW_EVENT);
  assert.deepEqual(events[0].detail, previewDetail);
  assert.equal(dispatchMermaidPreview(win, { ...previewDetail, width: -1 }), false);
  assert.equal(events.length, 1);
});
