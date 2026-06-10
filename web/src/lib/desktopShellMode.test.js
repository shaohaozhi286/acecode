// desktopShellMode 的单测。
//
// 背景(回归来源):webapp 兼容模式(Edge --app + ?ace_webapp=1)下,
// DesktopContextMenu 原来只认 WebView2 壳注入的 window.__ACECODE_DESKTOP_SHELL__ /
// aceDesktop_* bridge,兼容模式两者皆无 → 自定义右键菜单整体失效,用户只能看到
// Edge 原生菜单。修复 = 把 ace_webapp=1 固化后视作"类桌面"环境。
//
// 覆盖三块:
//   - detectWebappCompat:URL 参数与固化标志的判定优先级
//   - installWebappCompatFlag:固化到 sessionStorage + 暴露全局标志;storage 抛异常不致命
//   - isWebappCompat / desktopUiMode:跨 replaceState(query 已被抹掉)后仍能识别

import assert from 'node:assert/strict';
import {
  WEBAPP_COMPAT_STORAGE_KEY,
  detectWebappCompat,
  installWebappCompatFlag,
  isWebappCompat,
  isDesktopShell,
  desktopUiMode,
} from './desktopShellMode.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 内存 sessionStorage。throwOn 注入读 / 写失败(隐私模式下两者都可能抛)。
function makeMemStorage(initial = {}, throwOn = null) {
  const store = new Map(Object.entries(initial));
  return {
    getItem(k) {
      if (throwOn === 'get') throw new Error('get fail');
      return store.has(k) ? store.get(k) : null;
    },
    setItem(k, v) {
      if (throwOn === 'set') throw new Error('set fail');
      store.set(k, String(v));
    },
    _store: store,
  };
}

function makeWin({ search = '', storage = makeMemStorage(), shell = false } = {}) {
  const win = {
    location: { search },
    sessionStorage: storage,
  };
  if (shell) win.__ACECODE_DESKTOP_SHELL__ = true;
  return win;
}

run('detectWebappCompat:URL 带 ace_webapp=1 → true', () => {
  assert.equal(detectWebappCompat({ search: '?ace_webapp=1' }), true);
  assert.equal(detectWebappCompat({ search: '?token=abc&ace_webapp=1' }), true);
});

run('detectWebappCompat:无参数且无固化标志 → false(普通浏览器直连不得误判)', () => {
  assert.equal(detectWebappCompat({ search: '' }), false);
  assert.equal(detectWebappCompat({ search: '?token=abc' }), false);
  // 值不是 '1' 不算:避免 ?ace_webapp=0 之类被误认
  assert.equal(detectWebappCompat({ search: '?ace_webapp=0' }), false);
});

run('detectWebappCompat:固化标志存在时即使 URL 已被 replaceState 抹掉也 → true', () => {
  assert.equal(detectWebappCompat({ search: '', storedFlag: '1' }), true);
});

run('installWebappCompatFlag:首次进入固化到 sessionStorage 并设全局标志', () => {
  const storage = makeMemStorage();
  const win = makeWin({ search: '?ace_webapp=1', storage });
  assert.equal(installWebappCompatFlag(win), true);
  assert.equal(win.__ACECODE_WEBAPP_COMPAT__, true);
  assert.equal(storage._store.get(WEBAPP_COMPAT_STORAGE_KEY), '1');
});

run('installWebappCompatFlag:普通浏览器进入不写 storage 不设标志', () => {
  const storage = makeMemStorage();
  const win = makeWin({ search: '', storage });
  assert.equal(installWebappCompatFlag(win), false);
  assert.equal(win.__ACECODE_WEBAPP_COMPAT__, undefined);
  assert.equal(storage._store.has(WEBAPP_COMPAT_STORAGE_KEY), false);
});

run('installWebappCompatFlag:storage 写失败静默,本次会话仍有全局标志', () => {
  const win = makeWin({ search: '?ace_webapp=1', storage: makeMemStorage({}, 'set') });
  assert.equal(installWebappCompatFlag(win), true);
  assert.equal(win.__ACECODE_WEBAPP_COMPAT__, true);
});

run('installWebappCompatFlag:storage 读失败静默,URL 参数仍生效', () => {
  const win = makeWin({ search: '?ace_webapp=1', storage: makeMemStorage({}, 'get') });
  assert.equal(installWebappCompatFlag(win), true);
});

run('isWebappCompat:replaceState 抹掉 query 后凭 sessionStorage 仍识别(回归核心场景)', () => {
  const storage = makeMemStorage({ [WEBAPP_COMPAT_STORAGE_KEY]: '1' });
  const win = makeWin({ search: '', storage });
  assert.equal(isWebappCompat(win), true);
});

run('isWebappCompat:全局标志已设时不再读 storage', () => {
  const win = makeWin({ search: '', storage: makeMemStorage({}, 'get') });
  win.__ACECODE_WEBAPP_COMPAT__ = true;
  assert.equal(isWebappCompat(win), true);
});

run('isDesktopShell:认 __ACECODE_DESKTOP_SHELL__ 或任一 bridge 函数', () => {
  assert.equal(isDesktopShell(makeWin({ shell: true })), true);
  const bridged = makeWin({});
  bridged.aceDesktop_openInExplorer = () => {};
  assert.equal(isDesktopShell(bridged), true);
  assert.equal(isDesktopShell(makeWin({})), false);
});

run('desktopUiMode:shell 优先于 webapp,两者皆无 → browser', () => {
  const both = makeWin({ search: '?ace_webapp=1', shell: true });
  assert.equal(desktopUiMode(both), 'shell');
  const compat = makeWin({ search: '?ace_webapp=1' });
  assert.equal(desktopUiMode(compat), 'webapp');
  assert.equal(desktopUiMode(makeWin({})), 'browser');
});
