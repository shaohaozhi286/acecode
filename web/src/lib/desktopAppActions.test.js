import assert from 'node:assert/strict';
import { requestDesktopAppExit, showDesktopAboutDialog } from './desktopAppActions.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await test('desktop About action calls the native bridge and parses JSON', async () => {
  const result = await showDesktopAboutDialog({
    aceDesktop_showAboutDialog: async () => JSON.stringify({ ok: true }),
  });
  assert.deepEqual(result, { ok: true });
});

await test('desktop application actions report an unavailable bridge', async () => {
  assert.deepEqual(await showDesktopAboutDialog({}), { ok: false, unavailable: true });
  assert.deepEqual(await requestDesktopAppExit({}), { ok: false, unavailable: true });
});

await test('desktop exit action preserves native errors', async () => {
  const result = await requestDesktopAppExit({
    aceDesktop_quitApp: async () => JSON.stringify({ ok: false, error: 'quit failed' }),
  });
  assert.deepEqual(result, { ok: false, error: 'quit failed' });
});
