import assert from 'node:assert/strict';
import { copyImageToSystemClipboard, copyTextToSystemClipboard } from './systemClipboard.js';

function run(name, fn) {
  try {
    const ret = fn();
    if (ret && typeof ret.then === 'function') {
      return ret.then(
        () => console.log(`[pass] ${name}`),
        (err) => { console.error(`[fail] ${name}`); throw err; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('system clipboard uses desktop bridge first', async () => {
  const calls = [];
  const result = await copyTextToSystemClipboard('1BEA-E219', {
    aceDesktop_writeClipboardText: async (text) => {
      calls.push(text);
      return JSON.stringify({ ok: true });
    },
    navigator: {
      clipboard: {
        writeText: () => { throw new Error('navigator clipboard must not be used'); },
      },
    },
  });

  assert.deepEqual(calls, ['1BEA-E219']);
  assert.deepEqual(result, { ok: true, via: 'desktop' });
});

await run('system clipboard falls back to navigator outside desktop shell', async () => {
  const calls = [];
  const result = await copyTextToSystemClipboard('1BEA-E219', {
    navigator: {
      clipboard: {
        writeText: async (text) => { calls.push(text); },
      },
    },
  });

  assert.deepEqual(calls, ['1BEA-E219']);
  assert.deepEqual(result, { ok: true, via: 'navigator' });
});

await run('system clipboard reports desktop bridge failure when no fallback exists', async () => {
  const result = await copyTextToSystemClipboard('1BEA-E219', {
    aceDesktop_writeClipboardText: async () => JSON.stringify({ ok: false, error: 'native failed' }),
  });

  assert.equal(result.ok, false);
  assert.equal(result.error, 'native failed');
});

await run('image clipboard writes fetched image blob as ClipboardItem', async () => {
  const writes = [];
  const items = [];
  class FakeClipboardItem {
    constructor(payload) {
      items.push(payload);
      this.payload = payload;
    }
  }

  const result = await copyImageToSystemClipboard('/api/blob', {
    mimeType: 'image/png',
    fetchImpl: async (url) => ({
      ok: true,
      status: 200,
      blob: async () => new Blob(['png'], { type: 'image/png' }),
      url,
    }),
    win: {
      ClipboardItem: FakeClipboardItem,
      navigator: {
        clipboard: {
          write: async (payload) => { writes.push(payload); },
        },
      },
    },
  });

  assert.equal(result.ok, true);
  assert.equal(result.mimeType, 'image/png');
  assert.equal(writes.length, 1);
  assert.equal(writes[0][0] instanceof FakeClipboardItem, true);
  assert.equal(items[0]['image/png'].type, 'image/png');
});

await run('image clipboard converts non-png images when direct write is unsupported', async () => {
  const writes = [];
  const items = [];
  const draws = [];
  const closes = [];
  const convertedBlob = new Blob(['png'], { type: 'image/png' });
  class FakeClipboardItem {
    constructor(payload) {
      items.push(payload);
      this.payload = payload;
    }
  }

  const result = await copyImageToSystemClipboard('/api/photo', {
    mimeType: 'image/jpeg',
    fetchImpl: async () => ({
      ok: true,
      status: 200,
      blob: async () => new Blob(['jpeg'], { type: 'image/jpeg' }),
    }),
    win: {
      ClipboardItem: FakeClipboardItem,
      createImageBitmap: async () => ({
        width: 8,
        height: 6,
        close: () => { closes.push(true); },
      }),
      document: {
        createElement: () => ({
          width: 0,
          height: 0,
          getContext: () => ({
            drawImage: (...args) => { draws.push(args); },
          }),
          toBlob: (callback, type) => {
            assert.equal(type, 'image/png');
            callback(convertedBlob);
          },
        }),
      },
      navigator: {
        clipboard: {
          write: async (payload) => {
            writes.push(payload);
            if (writes.length === 1) throw new Error('image/jpeg unsupported');
          },
        },
      },
    },
  });

  assert.equal(result.ok, true);
  assert.equal(result.mimeType, 'image/png');
  assert.equal(result.converted, true);
  assert.equal(writes.length, 2);
  assert.equal(items[0]['image/jpeg'].type, 'image/jpeg');
  assert.equal(items[1]['image/png'].type, 'image/png');
  assert.equal(draws.length, 1);
  assert.deepEqual(closes, [true]);
});

await run('image clipboard reports unavailable binary clipboard support', async () => {
  const result = await copyImageToSystemClipboard('/api/blob', {
    win: {
      navigator: {
        clipboard: {
          writeText: async () => {},
        },
      },
    },
  });

  assert.equal(result.ok, false);
  assert.equal(result.error, 'image clipboard unavailable');
});

await run('image clipboard rejects non-image blobs', async () => {
  const result = await copyImageToSystemClipboard('/api/blob', {
    fetchImpl: async () => ({
      ok: true,
      blob: async () => new Blob(['text'], { type: 'text/plain' }),
    }),
    win: {
      ClipboardItem: class {},
      navigator: {
        clipboard: {
          write: async () => {},
        },
      },
    },
  });

  assert.equal(result.ok, false);
  assert.equal(result.error, 'clipboard item is not an image');
});
