import assert from 'node:assert/strict';
import {
  DESKTOP_CLOSE_BEHAVIORS,
  DESKTOP_CLOSE_REQUEST_EVENT,
  desktopCloseBehaviorAvailable,
  getDesktopCloseBehavior,
  hideDesktopToTray,
  normalizeDesktopCloseBehavior,
  performDesktopCloseChoice,
  setDesktopCloseBehavior,
  subscribeDesktopCloseRequest,
} from './desktopCloseBehavior.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function desktopBridge(overrides = {}) {
  return {
    __ACECODE_DESKTOP_SHELL__: true,
    aceDesktop_getCloseBehaviorPreference: async () => JSON.stringify({
      ok: true,
      behavior: 'ask',
      tray_available: true,
    }),
    aceDesktop_setCloseBehaviorPreference: async (behavior) => ({
      ok: true,
      behavior,
      tray_available: true,
    }),
    aceDesktop_hideToTray: async () => '{"ok":true}',
    ...overrides,
  };
}

await test('desktop close behavior bridges expose normalized state', async () => {
  const win = desktopBridge();
  assert.equal(desktopCloseBehaviorAvailable(win), true);
  assert.deepEqual(await getDesktopCloseBehavior(win), {
    ok: true,
    behavior: 'ask',
    tray_available: true,
    trayAvailable: true,
  });
  assert.equal(
    (await setDesktopCloseBehavior(DESKTOP_CLOSE_BEHAVIORS.EXIT, win)).behavior,
    DESKTOP_CLOSE_BEHAVIORS.EXIT,
  );
  assert.deepEqual(await hideDesktopToTray(win), { ok: true });
});

await test('desktop close behavior rejects unavailable and invalid calls', async () => {
  assert.equal((await getDesktopCloseBehavior({})).unavailable, true);
  assert.equal(
    (await setDesktopCloseBehavior('invalid', desktopBridge())).error,
    '无效的关闭窗口行为',
  );
  assert.equal(normalizeDesktopCloseBehavior('invalid'), DESKTOP_CLOSE_BEHAVIORS.ASK);
});

await test('close request subscription uses one stable native event', async () => {
  const calls = [];
  const win = {
    addEventListener: (name, handler) => calls.push(['add', name, handler]),
    removeEventListener: (name, handler) => calls.push(['remove', name, handler]),
  };
  const handler = () => {};
  const unsubscribe = subscribeDesktopCloseRequest(handler, win);
  unsubscribe();
  assert.deepEqual(calls, [
    ['add', DESKTOP_CLOSE_REQUEST_EVENT, handler],
    ['remove', DESKTOP_CLOSE_REQUEST_EVENT, handler],
  ]);
});

await test('remembered choice persists before its action', async () => {
  const order = [];
  const result = await performDesktopCloseChoice({
    behavior: DESKTOP_CLOSE_BEHAVIORS.MINIMIZE_TO_TRAY,
    remember: true,
    persist: async () => { order.push('persist'); return { ok: true }; },
    hideToTray: async () => { order.push('hide'); return { ok: true }; },
    exitApp: async () => { order.push('exit'); return { ok: true }; },
  });
  assert.deepEqual(order, ['persist', 'hide']);
  assert.equal(result.ok, true);
});

await test('unchecked choice is one-shot and persistence failure blocks action', async () => {
  const oneShot = [];
  await performDesktopCloseChoice({
    behavior: DESKTOP_CLOSE_BEHAVIORS.EXIT,
    remember: false,
    persist: async () => { oneShot.push('persist'); return { ok: true }; },
    hideToTray: async () => { oneShot.push('hide'); return { ok: true }; },
    exitApp: async () => { oneShot.push('exit'); return { ok: true }; },
  });
  assert.deepEqual(oneShot, ['exit']);

  const blocked = [];
  const failed = await performDesktopCloseChoice({
    behavior: DESKTOP_CLOSE_BEHAVIORS.EXIT,
    remember: true,
    persist: async () => ({ ok: false, error: 'save failed' }),
    hideToTray: async () => { blocked.push('hide'); return { ok: true }; },
    exitApp: async () => { blocked.push('exit'); return { ok: true }; },
  });
  assert.equal(failed.stage, 'persist');
  assert.deepEqual(blocked, []);
});

