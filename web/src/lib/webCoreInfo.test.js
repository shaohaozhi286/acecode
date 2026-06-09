import assert from 'node:assert/strict';
import {
  formatProgramVersion,
  formatWebCoreDetail,
  formatWebCoreLabel,
  getCurrentWebCoreInfo,
  normalizeDesktopWebCoreInfo,
  webCoreInfoFromUserAgent,
} from './webCoreInfo.js';

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

run('formatProgramVersion prefixes semantic versions', () => {
  assert.equal(formatProgramVersion('0.4.3'), 'v0.4.3');
  assert.equal(formatProgramVersion('v0.4.3'), 'v0.4.3');
  assert.equal(formatProgramVersion(''), '未知');
});

run('normalizeDesktopWebCoreInfo accepts webview bridge JSON string', () => {
  const info = normalizeDesktopWebCoreInfo(JSON.stringify({
    ok: true,
    backend: 'webview2',
    name: 'WebView2',
    version: '139.0.3405.86',
    detail: 'Evergreen Runtime',
    wrapper_name: 'webview',
    wrapper_version: '0.12.0',
  }));
  assert.equal(formatWebCoreLabel(info), 'WebView2 139.0.3405.86');
  assert.equal(formatWebCoreDetail(info), 'Evergreen Runtime · webview 0.12.0');
});

run('webCoreInfoFromUserAgent prefers Edge version over embedded Chrome token', () => {
  const info = webCoreInfoFromUserAgent(
    'Mozilla/5.0 AppleWebKit/537.36 Chrome/125.0.0.0 Safari/537.36 Edg/126.0.1.2',
  );
  assert.equal(info.name, 'Edge/Chromium');
  assert.equal(info.version, '126.0.1.2');
});

await run('getCurrentWebCoreInfo uses desktop bridge when available', async () => {
  const info = await getCurrentWebCoreInfo({
    aceDesktop_getWebCoreInfo: async () => JSON.stringify({
      ok: true,
      name: 'WebKitGTK',
      version: '2.44.1',
      detail: 'GTK 3.24.41',
    }),
    navigator: { userAgent: 'Chrome/1.0' },
  });
  assert.equal(info.source, 'desktop');
  assert.equal(formatWebCoreLabel(info), 'WebKitGTK 2.44.1');
});

await run('getCurrentWebCoreInfo falls back to user agent on bridge failure', async () => {
  const info = await getCurrentWebCoreInfo({
    aceDesktop_getWebCoreInfo: async () => JSON.stringify({ ok: false, error: 'native failed' }),
    navigator: { userAgent: 'Mozilla/5.0 AppleWebKit/605.1.15 Version/17.4 Safari/605.1.15' },
  });
  assert.equal(info.source, 'browser');
  assert.equal(info.name, 'Safari/WebKit');
  assert.equal(info.version, '17.4');
});
