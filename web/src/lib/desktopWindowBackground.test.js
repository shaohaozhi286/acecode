import assert from 'node:assert/strict';
import {
  normalizeThemeBackgroundColor,
  pushWindowBackgroundColor,
} from './desktopWindowBackground.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// ── normalizeThemeBackgroundColor ────────────────────────────────────────

// 场景:getComputedStyle 返回 CSS custom property 的声明字面量,带前导空白
// (' #f5f5f2' 是 --ace-bg 的真实 computed 形态)。期望:trim + 小写规范化。
test('computed style literal with leading space normalizes', () => {
  assert.equal(normalizeThemeBackgroundColor(' #f5f5f2'), '#f5f5f2');
});

// 场景:大写 hex / 无 # 前缀(上游 CSS 改写法)。期望:统一成 #rrggbb 小写。
test('uppercase and bare hex normalize to lowercase with hash', () => {
  assert.equal(normalizeThemeBackgroundColor('#0F0F0F'), '#0f0f0f');
  assert.equal(normalizeThemeBackgroundColor('F5F5F2'), '#f5f5f2');
});

// 场景:CSS 3 位短形(#f52 合法写法)。期望:逐位翻倍展开成 6 位 —
// native 端只收 6 位,展开是前端 normalize 的职责。
test('three digit shorthand expands', () => {
  assert.equal(normalizeThemeBackgroundColor('#f52'), '#ff5522');
});

// 场景:认不出的形态 — rgb() 函数 / 关键字 / 空串 / 4、5、7、8 位 hex /
// 非字符串。期望:一律 null(上游改了 --ace-bg 写法时宁可 no-op,也不把
// 猜测值推给 native 涂窗口)。
test('unrecognized forms return null', () => {
  assert.equal(normalizeThemeBackgroundColor('rgb(245, 245, 242)'), null);
  assert.equal(normalizeThemeBackgroundColor('white'), null);
  assert.equal(normalizeThemeBackgroundColor(''), null);
  assert.equal(normalizeThemeBackgroundColor('#f5f2'), null);
  assert.equal(normalizeThemeBackgroundColor('#f5f5f'), null);
  assert.equal(normalizeThemeBackgroundColor('#f5f5f21'), null);
  assert.equal(normalizeThemeBackgroundColor('#ff00ff00'), null);
  assert.equal(normalizeThemeBackgroundColor(undefined), null);
  assert.equal(normalizeThemeBackgroundColor(null), null);
});

// ── pushWindowBackgroundColor ────────────────────────────────────────────

function fakeEnv({ bridge, cssValue = ' #f5f5f2' } = {}) {
  const calls = [];
  const doc = { documentElement: {} };
  const win = {
    aceDesktop_setWindowBackgroundColor: bridge === undefined
      ? (color) => { calls.push(color); return Promise.resolve('{"ok":true}'); }
      : bridge,
    getComputedStyle: () => ({
      getPropertyValue: (name) => (name === '--ace-bg' ? cssValue : ''),
    }),
  };
  return { win, doc, calls };
}

// 场景:桌面壳内(bridge 存在)主题生效后调用。期望:读 --ace-bg、规范化后
// 推给 bridge,返回 true。
test('pushes normalized --ace-bg value through bridge', () => {
  const { win, doc, calls } = fakeEnv();
  assert.equal(pushWindowBackgroundColor(win, doc), true);
  assert.deepEqual(calls, ['#f5f5f2']);
});

// 场景:浏览器直连 / webapp 兼容模式 — window 上没有 bridge 函数。
// 期望:no-op 返回 false,不抛(desktopNotify 同款降级)。
test('missing bridge is a silent no-op', () => {
  const { win, doc } = fakeEnv({ bridge: null });
  win.aceDesktop_setWindowBackgroundColor = undefined;
  assert.equal(pushWindowBackgroundColor(win, doc), false);
});

// 场景:上游把 --ace-bg 改成 rgb() 形态导致 normalize 失败。期望:不调
// bridge,返回 false — 不把猜测值涂到窗口上。
test('unnormalizable css value skips the bridge call', () => {
  const { win, doc, calls } = fakeEnv({ cssValue: 'rgb(245, 245, 242)' });
  assert.equal(pushWindowBackgroundColor(win, doc), false);
  assert.deepEqual(calls, []);
});

// 场景:bridge 同步抛异常(壳侧 binding 异常)。期望:吞掉返回 false,
// 不打断 ThemeProvider 的 effect。
test('bridge throwing synchronously is swallowed', () => {
  const { win, doc } = fakeEnv({
    bridge: () => { throw new Error('shell exploded'); },
  });
  assert.equal(pushWindowBackgroundColor(win, doc), false);
});

// 场景:bridge 返回 rejected Promise(native 端 ok:false 之外的失败形态)。
// 期望:同步返回 true(已发起推送)且 rejection 被兜住,不产生
// unhandledRejection — fire-and-forget 语义。
test('rejected bridge promise is caught fire-and-forget', async () => {
  let caught = false;
  const rejected = {
    catch: (fn) => { caught = true; fn(new Error('native says no')); },
  };
  const { win, doc } = fakeEnv({ bridge: () => rejected });
  assert.equal(pushWindowBackgroundColor(win, doc), true);
  assert.equal(caught, true);
});

// 场景:win/doc 缺失(SSR / 非浏览器环境防御)。期望:false 不抛。
test('missing window or document is a no-op', () => {
  assert.equal(pushWindowBackgroundColor(undefined, undefined), false);
});
