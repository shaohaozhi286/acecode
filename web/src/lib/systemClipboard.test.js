import assert from 'node:assert/strict';
import { copyTextToSystemClipboard } from './systemClipboard.js';

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
