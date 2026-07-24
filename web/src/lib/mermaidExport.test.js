import assert from 'node:assert/strict';

import {
  downloadMermaidBlob,
  exportMermaidAsset,
  exportMermaidPng,
  exportMermaidSource,
  exportMermaidSvg,
  MAX_MERMAID_EXPORT_SOURCE_BYTES,
  MERMAID_EXPORT_TARGET_SELECTOR,
  MERMAID_PNG_MAX_DIMENSION,
  MERMAID_PNG_MAX_PIXELS,
  mermaidExportFilename,
  mermaidExportTargetFromElement,
  mermaidFamilyFromSource,
  mermaidPngDimensions,
  normalizeMermaidExportDetail,
  registerMermaidExportTarget,
  unregisterMermaidExportTarget,
} from './mermaidExport.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const detail = {
  source: 'flowchart TD\nA[Start] --> B[Done]',
  svg: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 80"></svg>',
  width: 160,
  height: 80,
  theme: 'dark',
};

function fakeElement() {
  const attrs = new Map();
  return {
    attrs,
    setAttribute(name, value) { attrs.set(name, String(value)); },
    removeAttribute(name) { attrs.delete(name); },
    getAttribute(name) { return attrs.get(name) || ''; },
    closest(selector) { return selector === MERMAID_EXPORT_TARGET_SELECTOR ? this : null; },
  };
}

function downloadHarness() {
  const anchors = [];
  const blobs = new Map();
  const revoked = [];
  let nextId = 1;
  const URLApi = {
    createObjectURL(blob) {
      const url = `blob:test-${nextId++}`;
      blobs.set(url, blob);
      return url;
    },
    revokeObjectURL(url) { revoked.push(url); },
  };
  const document = {
    body: { append() {} },
    createElement(tag) {
      assert.equal(tag, 'a');
      const anchor = {
        style: {},
        clickCalled: false,
        click() { this.clickCalled = true; },
        remove() {},
      };
      anchors.push(anchor);
      return anchor;
    },
  };
  return {
    anchors,
    blobs,
    revoked,
    options: { document, URLApi, schedule: (fn) => fn() },
  };
}

await test('Mermaid export payload is bounded and stored outside DOM attributes', () => {
  const normalized = normalizeMermaidExportDetail(detail);
  assert.equal(normalized.type, 'mermaid');
  assert.equal(normalized.theme, 'dark');
  assert.equal(normalizeMermaidExportDetail({
    ...detail,
    source: `flowchart TD\nA[${'x'.repeat(MAX_MERMAID_EXPORT_SOURCE_BYTES)}]`,
  }), null);

  const element = fakeElement();
  const registered = registerMermaidExportTarget(element, detail);
  assert.deepEqual(registered, normalized);
  assert.equal(element.getAttribute('data-mermaid-export-target'), 'true');
  assert.equal([...element.attrs.values()].some((value) => value.includes('<svg')), false);
  assert.equal(mermaidExportTargetFromElement(element), registered);
  unregisterMermaidExportTarget(element);
  assert.equal(element.getAttribute('data-mermaid-export-target'), '');
  assert.equal(mermaidExportTargetFromElement(element), null);
});

await test('Mermaid families produce deterministic export filenames', () => {
  const cases = [
    ['%% comment\nflowchart LR\nA --> B', 'flowchart'],
    ['stateDiagram-v2\n[*] --> A', 'state'],
    ['classDiagram\nclass A', 'class'],
    ['erDiagram\nA ||--|| B : has', 'er'],
    ['sequenceDiagram\nA->>B: hi', 'sequence'],
  ];
  for (const [source, family] of cases) {
    assert.equal(mermaidFamilyFromSource(source), family);
    assert.equal(mermaidExportFilename({ source }, 'svg'), `mermaid-${family}.svg`);
  }
  assert.equal(mermaidExportFilename(detail, 'source'), 'mermaid-flowchart.mmd');
});

await test('PNG dimensions prefer 2x but obey dimension and pixel caps', () => {
  assert.deepEqual(mermaidPngDimensions(160, 80), { width: 320, height: 160, scale: 2 });
  const large = mermaidPngDimensions(32768, 16384);
  assert.ok(large.width <= MERMAID_PNG_MAX_DIMENSION);
  assert.ok(large.height <= MERMAID_PNG_MAX_DIMENSION);
  assert.ok(large.width * large.height <= MERMAID_PNG_MAX_PIXELS);
  assert.ok(Math.abs((large.width / large.height) - 2) < 0.01);
  assert.equal(mermaidPngDimensions(0, 80), null);
});

await test('SVG and source export download exact retained artifacts and revoke URLs', async () => {
  const svgHarness = downloadHarness();
  const svgResult = exportMermaidSvg(detail, { BlobType: Blob, ...svgHarness.options });
  assert.deepEqual(svgResult, { ok: true, filename: 'mermaid-flowchart.svg' });
  assert.equal(svgHarness.anchors[0].download, 'mermaid-flowchart.svg');
  assert.equal(await svgHarness.blobs.get(svgHarness.anchors[0].href).text(), detail.svg);
  assert.deepEqual(svgHarness.revoked, [svgHarness.anchors[0].href]);

  const sourceHarness = downloadHarness();
  const sourceResult = exportMermaidSource(detail, { BlobType: Blob, ...sourceHarness.options });
  assert.deepEqual(sourceResult, { ok: true, filename: 'mermaid-flowchart.mmd' });
  assert.equal(await sourceHarness.blobs.get(sourceHarness.anchors[0].href).text(), detail.source);
  assert.deepEqual(sourceHarness.revoked, [sourceHarness.anchors[0].href]);
});

await test('Blob download revokes immediately when the anchor click fails', () => {
  const revoked = [];
  const URLApi = {
    createObjectURL: () => 'blob:broken',
    revokeObjectURL: (url) => revoked.push(url),
  };
  const result = downloadMermaidBlob(new Blob(['x']), 'x.txt', {
    URLApi,
    document: {
      body: { append() {} },
      createElement: () => ({
        style: {},
        click() { throw new Error('blocked'); },
        remove() {},
      }),
    },
  });
  assert.equal(result.ok, false);
  assert.match(result.error, /blocked/);
  assert.deepEqual(revoked, ['blob:broken']);
});

await test('PNG export paints the theme, downloads 2x output, and cleans both URLs', async () => {
  const harness = downloadHarness();
  const drawCalls = [];
  const canvas = {
    width: 0,
    height: 0,
    getContext() {
      return {
        fillStyle: '',
        fillRect(...args) { drawCalls.push(['fill', this.fillStyle, ...args]); },
        drawImage(...args) { drawCalls.push(['draw', ...args.slice(1)]); },
      };
    },
    toBlob(callback, type) { callback(new Blob(['png'], { type })); },
  };
  harness.options.document.createElement = (tag) => {
    if (tag === 'canvas') return canvas;
    const anchor = { style: {}, click() {}, remove() {} };
    harness.anchors.push(anchor);
    return anchor;
  };
  class FakeImage {
    set src(value) {
      this.value = value;
      queueMicrotask(() => this.onload?.());
    }
  }

  const result = await exportMermaidPng(detail, {
    BlobType: Blob,
    ImageType: FakeImage,
    ...harness.options,
  });
  assert.deepEqual(result, { ok: true, filename: 'mermaid-flowchart.png' });
  assert.equal(canvas.width, 320);
  assert.equal(canvas.height, 160);
  assert.deepEqual(drawCalls[0], ['fill', '#333333', 0, 0, 320, 160]);
  assert.deepEqual(drawCalls[1], ['draw', 0, 0, 320, 160]);
  assert.equal(harness.revoked.length, 2);
});

await test('PNG decode failure revokes its SVG URL without starting a download', async () => {
  const harness = downloadHarness();
  class BrokenImage {
    set src(value) {
      this.value = value;
      queueMicrotask(() => this.onerror?.());
    }
  }
  const result = await exportMermaidPng(detail, {
    BlobType: Blob,
    ImageType: BrokenImage,
    ...harness.options,
  });
  assert.equal(result.ok, false);
  assert.match(result.error, /decode failed/);
  assert.equal(harness.anchors.length, 0);
  assert.equal(harness.revoked.length, 1);
});

await test('generic Mermaid exporter rejects unsupported formats', async () => {
  const result = await exportMermaidAsset(detail, 'jpeg');
  assert.equal(result.ok, false);
  assert.match(result.error, /unsupported/);
});
