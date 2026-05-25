import assert from 'node:assert/strict';
import { openExternalUrl } from './externalUrl.js';

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

await run('desktop external URL opener uses native bridge', async () => {
  const calls = [];
  const result = await openExternalUrl('https://github.com/login/device', {
    aceDesktop_openExternalUrl: async (url) => {
      calls.push(url);
      return JSON.stringify({ ok: true });
    },
    open: () => { throw new Error('window.open must not be used'); },
  });

  assert.deepEqual(calls, ['https://github.com/login/device']);
  assert.deepEqual(result, { ok: true, via: 'desktop' });
});

await run('desktop external URL opener does not fallback to window.open on bridge failure', async () => {
  let windowOpenCalls = 0;
  const result = await openExternalUrl('https://github.com/login/device', {
    aceDesktop_openExternalUrl: async () => JSON.stringify({ ok: false, error: 'native failed' }),
    open: () => { windowOpenCalls += 1; },
  });

  assert.equal(result.ok, false);
  assert.equal(result.via, 'desktop');
  assert.equal(result.error, 'native failed');
  assert.equal(windowOpenCalls, 0);
});

await run('external URL opener falls back to window.open outside desktop shell', async () => {
  const calls = [];
  const result = await openExternalUrl('https://github.com/login/device', {
    open: (...args) => { calls.push(args); },
  });

  assert.equal(result.ok, true);
  assert.equal(result.via, 'window-open');
  assert.deepEqual(calls, [['https://github.com/login/device', '_blank', 'noopener,noreferrer']]);
});
