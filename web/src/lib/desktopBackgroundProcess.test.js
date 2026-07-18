import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  desktopBackgroundProcessAvailable,
  getDesktopBackgroundProcess,
  setDesktopBackgroundProcess,
} from './desktopBackgroundProcess.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function bridge(extra = {}) {
  return {
    __ACECODE_DESKTOP_SHELL__: true,
    aceDesktop_getBackgroundProcessPreference: async () =>
      '{"ok":true,"enabled":false}',
    aceDesktop_setBackgroundProcessPreference: async (enabled) =>
      JSON.stringify({ ok: true, enabled }),
    ...extra,
  };
}

await test('background process preference is available only through Desktop bridge', async () => {
  assert.equal(desktopBackgroundProcessAvailable(bridge()), true);
  assert.equal(desktopBackgroundProcessAvailable({
    aceDesktop_getBackgroundProcessPreference() {},
    aceDesktop_setBackgroundProcessPreference() {},
  }), false);
  assert.equal(desktopBackgroundProcessAvailable({}), false);
});

await test('reads disabled preference without enabling it by default', async () => {
  assert.deepEqual(await getDesktopBackgroundProcess(bridge()), {
    ok: true,
    enabled: false,
  });
});

await test('writes boolean preference and returns native confirmation', async () => {
  let received = null;
  const win = bridge({
    aceDesktop_setBackgroundProcessPreference: async (enabled) => {
      received = enabled;
      return { ok: true, enabled };
    },
  });
  const state = await setDesktopBackgroundProcess(true, win);
  assert.equal(received, true);
  assert.equal(state.ok, true);
  assert.equal(state.enabled, true);
});

await test('browser-only callers receive an unavailable state', async () => {
  assert.deepEqual(await getDesktopBackgroundProcess({}), {
    ok: false,
    enabled: false,
    unavailable: true,
  });
  assert.deepEqual(await setDesktopBackgroundProcess(true, {}), {
    ok: false,
    enabled: false,
    unavailable: true,
  });
});

await test('Settings uses approved copy and gates the switch on native availability', async () => {
  const source = readFileSync(
    new URL('../components/SettingsPage.jsx', import.meta.url),
    'utf8',
  );
  assert.match(source, /后台进程状态/);
  assert.match(source, /退出 ACECode 后继续运行后台进程/);
  assert.match(source, /\{backgroundProcessAvailable && \(/);
  assert.match(source, /aria-checked=\{backgroundProcessEnabled\}/);
});
