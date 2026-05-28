import assert from 'node:assert/strict';
import {
  filesFromClipboardEvent,
  filesFromTransfer,
  hasFileTransfer,
} from './composerFileTransfer.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function fileLike(name, type = 'application/octet-stream') {
  return { name, type, size: 1 };
}

function list(items) {
  const out = { length: items.length, item: (index) => items[index] || null };
  items.forEach((item, index) => {
    out[index] = item;
  });
  return out;
}

run('drop transfer reads external files from FileList', () => {
  const png = fileLike('shot.png', 'image/png');
  const txt = fileLike('notes.txt', 'text/plain');
  const transfer = { files: list([png, txt]), items: list([]) };

  assert.equal(hasFileTransfer(transfer), true);
  assert.deepEqual(filesFromTransfer(transfer), [png, txt]);
});

run('item list fallback extracts clipboard image files', () => {
  const png = fileLike('clipboard.png', 'image/png');
  const transfer = {
    files: list([]),
    items: list([
      { kind: 'string', type: 'text/plain', getAsFile: () => null },
      { kind: 'file', type: 'image/png', getAsFile: () => png },
    ]),
  };

  assert.equal(hasFileTransfer(transfer), true);
  assert.deepEqual(filesFromTransfer(transfer, { source: 'paste' }), [png]);
});

run('text-only clipboard paste is ignored', () => {
  const event = {
    clipboardData: {
      files: list([]),
      items: list([{ kind: 'string', type: 'text/plain' }]),
    },
  };

  assert.equal(hasFileTransfer(event.clipboardData), false);
  assert.deepEqual(filesFromClipboardEvent(event), []);
});

run('unnamed pasted images get a stable filename when File is available', () => {
  if (typeof File !== 'function' || typeof Blob !== 'function') return;

  const unnamed = new File([new Blob(['x'], { type: 'image/png' })], '', { type: 'image/png' });
  const event = {
    clipboardData: {
      files: list([]),
      items: list([{ kind: 'file', type: 'image/png', getAsFile: () => unnamed }]),
    },
  };

  const files = filesFromClipboardEvent(event);
  assert.equal(files.length, 1);
  assert.equal(files[0].name, 'pasted-image.png');
  assert.equal(files[0].type, 'image/png');
});
