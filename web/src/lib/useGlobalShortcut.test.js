// useGlobalShortcut 的纯逻辑测试:hook 本体依赖 React + window,直接在 Node 跑代价大。
// 抽出 matchShortcut 纯函数后,这里只测匹配规则:Ctrl+K / Cmd+K 命中、其它键不命中、
// 缺修饰键不命中、key 大小写不敏感。

import assert from 'node:assert/strict';
import { matchShortcut } from './useGlobalShortcut.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const SPEC_CTRL_OR_META_K = { key: 'k', ctrl: true, meta: true };

run('Ctrl+K 命中', () => {
  assert.equal(matchShortcut({ key: 'k', ctrlKey: true,  metaKey: false }, SPEC_CTRL_OR_META_K), true);
});

run('Cmd+K(metaKey)命中', () => {
  assert.equal(matchShortcut({ key: 'k', ctrlKey: false, metaKey: true }, SPEC_CTRL_OR_META_K), true);
});

run('K 大写也命中(case-insensitive)', () => {
  assert.equal(matchShortcut({ key: 'K', ctrlKey: true, metaKey: false }, SPEC_CTRL_OR_META_K), true);
});

run('裸 K(无修饰键)不命中', () => {
  assert.equal(matchShortcut({ key: 'k', ctrlKey: false, metaKey: false }, SPEC_CTRL_OR_META_K), false);
});

run('Ctrl+J 不命中(key 不同)', () => {
  assert.equal(matchShortcut({ key: 'j', ctrlKey: true }, SPEC_CTRL_OR_META_K), false);
});

run('Shift 修饰要求生效', () => {
  const spec = { key: 'p', ctrl: true, shift: true };
  assert.equal(matchShortcut({ key: 'p', ctrlKey: true, shiftKey: true  }, spec), true);
  assert.equal(matchShortcut({ key: 'p', ctrlKey: true, shiftKey: false }, spec), false);
});

run('event 为 null 不抛错', () => {
  assert.equal(matchShortcut(null, SPEC_CTRL_OR_META_K), false);
});

run('spec 为 null 不抛错', () => {
  assert.equal(matchShortcut({ key: 'k', ctrlKey: true }, null), false);
});

run('event.key 非字符串(如 undefined)安全返回 false', () => {
  assert.equal(matchShortcut({ ctrlKey: true }, SPEC_CTRL_OR_META_K), false);
});
