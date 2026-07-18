import assert from 'node:assert/strict';
import {
  NOTIFICATION_AUTHORIZATION_EVENT,
  getMacNotificationAuthorization,
  macNotificationAuthorizationAvailable,
  normalizeNotificationAuthorization,
  notificationAuthorizationPresentation,
  openMacNotificationSettings,
  requestMacNotificationAuthorization,
  subscribeMacNotificationAuthorization,
} from './desktopNotificationAuthorization.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function macBridge(extra = {}) {
  return {
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'macos',
    aceDesktop_getNotificationAuthorization: async () => JSON.stringify({
      ok: true,
      status: 'not_determined',
      can_request: true,
      can_open_settings: true,
    }),
    ...extra,
  };
}

await test('normalizes native authorization JSON', async () => {
  assert.deepEqual(
    normalizeNotificationAuthorization(JSON.stringify({
      ok: true,
      status: 'authorized',
      can_request: false,
      can_open_settings: true,
    })),
    {
      ok: true,
      status: 'authorized',
      canRequest: false,
      canOpenSettings: true,
    },
  );
});

await test('unknown native status safely becomes unavailable', async () => {
  assert.equal(normalizeNotificationAuthorization({ status: 'mystery' }).status, 'unavailable');
  assert.equal(normalizeNotificationAuthorization(null).ok, false);
});

await test('authorization bridge is macOS Desktop only', async () => {
  assert.equal(macNotificationAuthorizationAvailable(macBridge()), true);
  assert.equal(macNotificationAuthorizationAvailable({
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'windows',
    aceDesktop_getNotificationAuthorization() {},
  }), false);
  assert.equal(macNotificationAuthorizationAvailable({}), false);
});

await test('queries and requests authorization through native bridges', async () => {
  const win = macBridge({
    aceDesktop_requestNotificationAuthorization: async () => ({
      ok: true,
      status: 'requesting',
      can_request: false,
      can_open_settings: true,
    }),
  });
  assert.equal((await getMacNotificationAuthorization(win)).status, 'not_determined');
  assert.equal((await requestMacNotificationAuthorization(win)).status, 'requesting');
});

await test('opens System Settings only when native bridge succeeds', async () => {
  assert.equal(await openMacNotificationSettings(macBridge({
    aceDesktop_openNotificationSettings: async () => '{"ok":true}',
  })), true);
  assert.equal(await openMacNotificationSettings(macBridge({
    aceDesktop_openNotificationSettings: async () => '{"ok":false}',
  })), false);
  assert.equal(await openMacNotificationSettings({}), false);
});

await test('subscription normalizes native DOM event details', async () => {
  let registered = null;
  let removed = null;
  let seen = null;
  const win = {
    addEventListener: (name, fn) => { registered = { name, fn }; },
    removeEventListener: (name, fn) => { removed = { name, fn }; },
  };
  const unsubscribe = subscribeMacNotificationAuthorization(
    (state) => { seen = state; },
    win,
  );
  assert.equal(registered.name, NOTIFICATION_AUTHORIZATION_EVENT);
  registered.fn({ detail: { status: 'denied', can_open_settings: true } });
  assert.equal(seen.status, 'denied');
  assert.equal(seen.canOpenSettings, true);
  unsubscribe();
  assert.equal(removed.name, NOTIFICATION_AUTHORIZATION_EVENT);
  assert.equal(removed.fn, registered.fn);
});

await test('presentation distinguishes authorization from app config', async () => {
  assert.equal(notificationAuthorizationPresentation({ status: 'authorized' }).tone, 'ok');
  assert.equal(notificationAuthorizationPresentation({ status: 'denied' }).tone, 'danger');
  assert.equal(notificationAuthorizationPresentation({ status: 'not_determined' }).tone, 'warn');
});
