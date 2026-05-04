// usePreference 的纯函数行为测试。
//
// hook 本体依赖 React state,直接在 Node 单测环境跑代价大。所以把读 / 写 /
// 合并三块抽成纯函数(readWithFallback / writeSafely / mergeNextValue)单独覆盖,
// 这正是这套偏好基础设施真正脆弱的部分:
//   - localStorage 不可用时不抛
//   - JSON 损坏 / 校验失败时回退默认
//   - setter 的 partial 浅合并语义 vs 整体替换语义

import assert from 'node:assert/strict';
import {
  readWithFallback,
  writeSafely,
  mergeNextValue,
} from './usePreference.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 内存 storage 模拟 localStorage 接口。throwOn 用来注入失败模式。
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

// readWithFallback ─────────────────────────────────────────

run('readWithFallback: 缺省 key 返回 defaults', () => {
  const storage = makeMemStorage();
  const v = readWithFallback('k', { a: 1 }, undefined, storage);
  assert.deepEqual(v, { a: 1 });
});

run('readWithFallback: 已有 key 返回反序列化值', () => {
  const storage = makeMemStorage({ k: JSON.stringify({ a: 2 }) });
  const v = readWithFallback('k', { a: 1 }, undefined, storage);
  assert.deepEqual(v, { a: 2 });
});

run('readWithFallback: JSON 损坏回退 defaults', () => {
  const storage = makeMemStorage({ k: '{not json' });
  const v = readWithFallback('k', { a: 1 }, undefined, storage);
  assert.deepEqual(v, { a: 1 });
});

run('readWithFallback: validator 拒绝时整体回退', () => {
  const storage = makeMemStorage({ k: JSON.stringify({ a: 'wrong' }) });
  const v = readWithFallback('k', { a: 1 }, (x) => typeof x?.a === 'number', storage);
  assert.deepEqual(v, { a: 1 });
});

run('readWithFallback: getItem 抛异常不传播,回退 defaults', () => {
  const storage = makeMemStorage({}, 'get');
  const v = readWithFallback('k', { a: 1 }, undefined, storage);
  assert.deepEqual(v, { a: 1 });
});

run('readWithFallback: storage 不可用(undefined)时回退 defaults', () => {
  const v = readWithFallback('k', { a: 1 }, undefined, undefined);
  assert.deepEqual(v, { a: 1 });
});

run('readWithFallback: 字符串值正常读取', () => {
  const storage = makeMemStorage({ theme: JSON.stringify('dark') });
  const v = readWithFallback('theme', 'light', (x) => x === 'light' || x === 'dark', storage);
  assert.equal(v, 'dark');
});

// writeSafely ──────────────────────────────────────────────

run('writeSafely: 正常写入 JSON 字符串', () => {
  const storage = makeMemStorage();
  const ok = writeSafely('k', { a: 1 }, storage);
  assert.equal(ok, true);
  assert.equal(storage._store.get('k'), '{"a":1}');
});

run('writeSafely: setItem 抛异常时返回 false 不传播', () => {
  const storage = makeMemStorage({}, 'set');
  const ok = writeSafely('k', { a: 1 }, storage);
  assert.equal(ok, false);
});

run('writeSafely: storage 不可用时返回 false', () => {
  const ok = writeSafely('k', { a: 1 }, undefined);
  assert.equal(ok, false);
});

// mergeNextValue ───────────────────────────────────────────

run('mergeNextValue: partial 对象与 prev 浅合并', () => {
  const next = mergeNextValue({ a: 1, b: 2 }, { b: 3 });
  assert.deepEqual(next, { a: 1, b: 3 });
});

run('mergeNextValue: 函数 updater 透传给 prev', () => {
  const next = mergeNextValue({ a: 1 }, (p) => ({ ...p, b: 2 }));
  assert.deepEqual(next, { a: 1, b: 2 });
});

run('mergeNextValue: 非对象 updater(string)整体替换', () => {
  const next = mergeNextValue('light', 'dark');
  assert.equal(next, 'dark');
});

run('mergeNextValue: prev 为非对象时不浅合并,整体替换', () => {
  const next = mergeNextValue('light', { theme: 'dark' });
  assert.deepEqual(next, { theme: 'dark' });
});

run('mergeNextValue: 数组 updater 整体替换不浅合并', () => {
  const next = mergeNextValue({ a: 1 }, [1, 2]);
  assert.deepEqual(next, [1, 2]);
});

run('mergeNextValue: null updater 整体替换', () => {
  const next = mergeNextValue({ a: 1 }, null);
  assert.equal(next, null);
});
